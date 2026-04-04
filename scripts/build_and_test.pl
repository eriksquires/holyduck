#!/usr/bin/env perl
# HolyDuck — build, deploy, regression test, self-diagnose on failure.
# Usage: perl build_and_test.pl [--container NAME]
# Prints "OK" on success, diagnostic report on failure.
use strict;
use warnings;
use DBI;
use File::Basename qw(dirname basename);
use Cwd            qw(abs_path);
use POSIX          qw(strftime);

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
my $ROOT       = abs_path(dirname(abs_path($0)) . "/..");
my $CONTAINER  = "duckdb-plugin-dev-ubuntu";
my $PLUGIN_OUT = "$ROOT/plugin-out-ubuntu";
my $BUILD_DIR  = "$ROOT/build";
my $DB_HOST    = "127.0.0.1";
my $DB_PORT    = 3306;

for my $arg (@ARGV) {
    if ($arg =~ /^--container=?(.+)/) {
        $CONTAINER = $1;
        # Derive distro from container name
        if ($CONTAINER =~ /oracle9/) {
            $PLUGIN_OUT = "$ROOT/plugin-out-oracle9";
            $BUILD_DIR  = "$ROOT/build-oracle9";
        } elsif ($CONTAINER =~ /oracle8/) {
            $PLUGIN_OUT = "$ROOT/plugin-out-oracle8";
            $BUILD_DIR  = "$ROOT/build-oracle8";
        } elsif ($CONTAINER =~ /debian/) {
            $PLUGIN_OUT = "$ROOT/plugin-out-debian12";
        }
    }
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
sub run {
    my ($cmd) = @_;
    my $out = `$cmd 2>&1`;
    return ($? == 0, $out);
}

sub connect_db {
    # Try with password first, then without (fresh/reset container)
    my $dbh = DBI->connect(
        "DBI:MariaDB:host=$DB_HOST;port=$DB_PORT",
        'root', 'testpass',
        { RaiseError => 0, PrintError => 0 }
    );
    return $dbh if $dbh;
    return DBI->connect(
        "DBI:MariaDB:host=$DB_HOST;port=$DB_PORT",
        'root', '',
        { RaiseError => 0, PrintError => 0 }
    );
}

sub sql {
    my ($dbh, $query) = @_;
    my $sth = $dbh->prepare($query);
    return (0, $dbh->errstr) unless $sth;
    unless ($sth->execute()) {
        return (0, $sth->errstr);
    }
    my @rows;
    if ($sth->{NUM_OF_FIELDS}) {
        while (my @r = $sth->fetchrow_array) {
            push @rows, join("\t", map { $_ // '' } @r);
        }
    }
    $sth->finish();
    return (1, join("\n", @rows));
}

sub section { print "\n=== $_[0] ===\n" }

sub fail {
    my ($msg) = @_;
    print "FAIL: $msg\n";
    diagnostics();
    exit 1;
}

# ---------------------------------------------------------------------------
# Diagnostics — run automatically on any failure
# ---------------------------------------------------------------------------
sub diagnostics {
    section "DIAGNOSTICS";

    my ($ok, $out) = run(
        "docker inspect --format '{{.State.Running}}' $CONTAINER 2>/dev/null"
    );
    my $running = ($out =~ /true/);
    print "Container running : " . ($running ? "yes" : "NO") . "\n";
    return unless $running;

    my $dbh = connect_db();
    if ($dbh) {
        my (undef, $ver) = sql($dbh, "SELECT VERSION()");
        chomp($ver //= '');
        print "MariaDB version  : $ver\n";

        (undef, $out) = sql($dbh, "SELECT name, dl FROM mysql.plugin WHERE name='duckdb'");
        chomp($out //= '');
        print "mysql.plugin row : " . ($out || "(not present)") . "\n";

        (undef, $out) = sql($dbh, "SELECT ENGINE, SUPPORT FROM information_schema.ENGINES WHERE ENGINE='DUCKDB'");
        chomp($out //= '');
        print "SHOW ENGINES row : " . ($out || "(not present)") . "\n";

        $dbh->disconnect;
    }

    my $so = "$PLUGIN_OUT/ha_duckdb.so";
    if (-f $so) {
        my $mtime = strftime("%Y-%m-%d %H:%M:%S", localtime((stat $so)[9]));
        printf ".so on host      : %d bytes  mtime=%s\n", -s $so, $mtime;
    } else {
        print ".so on host      : MISSING ($so)\n";
    }

    my $obj = "$BUILD_DIR/CMakeFiles/ha_duckdb.dir/ha_duckdb.cc.o";
    if (-f $obj) {
        my $mtime = strftime("%Y-%m-%d %H:%M:%S", localtime((stat $obj)[9]));
        print "Last .o mtime    : $mtime\n";
    }
}

# ---------------------------------------------------------------------------
# 1. Build — detect cmake config drift and reconfigure if needed
# ---------------------------------------------------------------------------
section "BUILD";

# Force recompile by deleting the object file
my $obj = "$BUILD_DIR/CMakeFiles/ha_duckdb.dir/ha_duckdb.cc.o";
unlink $obj if -f $obj;

# Check if cmake is configured with the right output directory
my $cache = "$BUILD_DIR/CMakeCache.txt";
my $configured_out = '';
if (open my $cf, '<', $cache) {
    while (<$cf>) {
        $configured_out = $1 if /CMAKE_LIBRARY_OUTPUT_DIRECTORY[^=]*=(.+)/;
    }
    close $cf;
}
chomp $configured_out;

# Container must be running for cmake and make (they run inside it)
my ($ok, $out) = run(
    "docker inspect --format '{{.State.Running}}' $CONTAINER 2>/dev/null"
);
my $was_running = ($out =~ /true/);
if (!$was_running) {
    # Stop other dev containers first (shared port 3306)
    my @others = grep { !/\Q$CONTAINER\E/ }
                 split /\n/, `docker ps --format '{{.Names}}' | grep '^duckdb-plugin-dev-'`;
    for my $c (@others) {
        chomp $c;
        print "Stopping $c to free port 3306...\n";
        run("docker stop $c");
    }
    print "Starting $CONTAINER...\n";
    run("docker start $CONTAINER");

    # Wait for MariaDB
    my $ready = 0;
    for (1..30) {
        sleep 1;
        my $dbh = connect_db();
        if ($dbh) {
            my ($ok, undef) = sql($dbh, "SELECT 1");
            if ($ok) { $ready = 1; $dbh->disconnect; last; }
            $dbh->disconnect;
        }
    }
    fail("MariaDB did not come up within 30 seconds") unless $ready;
}
print "Container: $CONTAINER — running\n";

# The plugin dir inside the container (bind-mounted to $PLUGIN_OUT on host)
my $plugin_dest = "/usr/lib/mysql/plugin";

# Reconfigure cmake if output directory doesn't point to the plugin dir
if ($configured_out ne $plugin_dest) {
    print "Reconfiguring cmake (output was '$configured_out', want '$plugin_dest')...\n";
    my $build_subdir = basename($BUILD_DIR);

    # Detect MariaDB build subdir from existing cache or default
    my $mariadb_build = "build";
    if (open my $cf2, '<', $cache) {
        while (<$cf2>) {
            $mariadb_build = $1 if m{MARIADB_BUILD_DIR[^=]*=/mariadb-src/(.+)};
        }
        close $cf2;
    }

    my ($rok, $rout) = run(
        "docker exec $CONTAINER bash -c '"
        . "mkdir -p /plugin-src/$build_subdir && "
        . "cmake /plugin-src/src "
        . "  -B /plugin-src/$build_subdir "
        . "  -DMARIADB_SOURCE_DIR=/mariadb-src "
        . "  -DMARIADB_BUILD_DIR=/mariadb-src/$mariadb_build "
        . "  -DCMAKE_BUILD_TYPE=RelWithDebInfo "
        . "  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$plugin_dest "
        . "  2>&1'"
    );
    fail("cmake reconfigure failed:\n$rout") unless $rok;
    print "cmake reconfigured.\n";
}

# Build
my $build_subdir = basename($BUILD_DIR);
($ok, $out) = run(
    "docker exec $CONTAINER make -j\$(nproc) -C /plugin-src/$build_subdir"
);
my $build_ok = $ok && $out !~ /\berror:/i;
for my $line (split /\n/, $out) {
    print "$line\n" if $line =~ /Building|Linking|error:|warning:/;
}
fail("build failed:\n$out") unless $build_ok;

fail(".so not found after build: $PLUGIN_OUT/ha_duckdb.so")
    unless -f "$PLUGIN_OUT/ha_duckdb.so";

my $so_mtime = strftime("%H:%M:%S", localtime((stat "$PLUGIN_OUT/ha_duckdb.so")[9]));
printf "Built: ha_duckdb.so (%d bytes, %s)\n", -s "$PLUGIN_OUT/ha_duckdb.so", $so_mtime;

# ---------------------------------------------------------------------------
# 2. Restart MariaDB to load the new binary
# ---------------------------------------------------------------------------
section "DEPLOY";

# Ensure bind-address=0.0.0.0 so DBI can connect from the host
run("docker exec $CONTAINER bash -c '"
  . "if ! grep -q bind-address /etc/mysql/mariadb.conf.d/99-holyduck.cnf 2>/dev/null; then "
  . "  printf \"[mysqld]\\nbind-address = 0.0.0.0\\n\" > /etc/mysql/mariadb.conf.d/99-holyduck.cnf; "
  . "fi'");

# Ensure root can connect from any host (not just localhost).
# MariaDB may auto-create a root@<docker-ip> entry with no password;
# that entry wins over root@% by host specificity, so set the password
# on all root entries.  Try with password first, then without (fresh container).
run("docker exec $CONTAINER mariadb -uroot -ptestpass --ssl=0 -e \""
  . "GRANT ALL ON *.* TO 'root'\@'%' IDENTIFIED BY 'testpass'; "
  . "UPDATE mysql.user SET password=PASSWORD('testpass') WHERE user='root' AND password=''; "
  . "FLUSH PRIVILEGES;\" 2>/dev/null")
or run("docker exec $CONTAINER mariadb -uroot --ssl=0 -e \""
  . "GRANT ALL ON *.* TO 'root'\@'%' IDENTIFIED BY 'testpass'; "
  . "UPDATE mysql.user SET password=PASSWORD('testpass') WHERE user='root' AND password=''; "
  . "FLUSH PRIVILEGES;\" 2>/dev/null");

print "Bouncing container...\n";
run("docker restart $CONTAINER");
sleep 2;  # let container init settle

my $ready = 0;
for (1..30) {
    run("docker exec $CONTAINER service mariadb start 2>/dev/null");
    sleep 1;
    my $dbh = connect_db();
    if ($dbh) {
        my ($ok, undef) = sql($dbh, "SELECT 1");
        if ($ok) { $ready = 1; $dbh->disconnect; last; }
        $dbh->disconnect;
    }
}
fail("MariaDB did not come up after restart") unless $ready;

# Ensure root password is set on all hosts (may be blank after container restart)
{
    my $dbh = connect_db();
    if ($dbh) {
        $dbh->do("GRANT ALL ON *.* TO 'root'\@'%' IDENTIFIED BY 'testpass'");
        $dbh->do("UPDATE mysql.user SET password=PASSWORD('testpass') WHERE user='root'");
        $dbh->do("FLUSH PRIVILEGES");
        $dbh->disconnect;
    }
}

# Install plugin if needed
{
    my $dbh = connect_db();
    fail("Cannot connect to MariaDB") unless $dbh;

    my (undef, $support) = sql($dbh,
        "SELECT SUPPORT FROM information_schema.ENGINES WHERE ENGINE='DUCKDB'");
    chomp($support //= '');
    if ($support ne 'YES') {
        sql($dbh, "INSTALL PLUGIN duckdb SONAME 'ha_duckdb.so'");
        (undef, $support) = sql($dbh,
            "SELECT SUPPORT FROM information_schema.ENGINES WHERE ENGINE='DUCKDB'");
        chomp($support //= '');
    }
    fail("DuckDB engine support='$support' (expected YES)") unless $support eq 'YES';
    print "Plugin loaded: YES\n";

    # Verify version and build freshness
    my (undef, $status) = sql($dbh, "SHOW ENGINE DUCKDB STATUS");
    my $build_ts;
    if ($status) {
        for my $line (split /\n/, $status) {
            print "  $line\n" if $line =~ /HolyDuck|DuckDB.*version/i;
            $build_ts = $1 if $line =~ /HolyDuck built\t(.+)/;
        }
    }

    if ($build_ts) {
        my %mon = (Jan=>0,Feb=>1,Mar=>2,Apr=>3,May=>4,Jun=>5,
                   Jul=>6,Aug=>7,Sep=>8,Oct=>9,Nov=>10,Dec=>11);
        if ($build_ts =~ /^(\w{3})\s+(\d+)\s+(\d{4})\s+(\d+):(\d+):(\d+)$/) {
            my $build_epoch = POSIX::mktime($6,$5,$4,$2,$mon{$1},$3-1900);
            my $age = time() - $build_epoch;
            $age = 0 if $age < 0;
            my $mins = int($age / 60);
            my $secs = $age % 60;
            printf "  Build age: %dm %ds\n", $mins, $secs;
            if ($age > 300) {
                print "  WARNING: Plugin binary is more than 5 minutes old!\n";
            }
        }
    } else {
        print "  WARNING: HolyDuck build timestamp not found in engine status\n";
    }

    $dbh->disconnect;
}

# ---------------------------------------------------------------------------
# 3. Regression tests
# ---------------------------------------------------------------------------
section "REGRESSION";

my $reg_dir = "$ROOT/tests/regression";
my $reg_run = "$reg_dir/run.pl";
if (-d $reg_dir && -f $reg_run) {
    my $result = system("perl $reg_run --host=$DB_HOST --port=$DB_PORT");
    fail("regression suite failed") if $result != 0;
} else {
    print "No regression suite found at $reg_dir — skipping\n";
}

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
print "\nOK\n";
