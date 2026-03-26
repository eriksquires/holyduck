#!/usr/bin/env perl
# run.pl — TPC-H performance harness for HolyDuck
#
# Runs all 22 TPC-H queries in three modes:
#   standalone  DuckDB CLI directly against global.duckdb (no MariaDB)
#   sm          All-DuckDB tables via MariaDB (measures plugin overhead)
#   mm          Mixed engine via MariaDB (DuckDB facts + InnoDB dimensions)
#
# Each query: 1 warmup run (discarded) + N timed runs (averaged).
# Failed or timed-out queries are recorded as FAILED/TIMEOUT, not fatal.
#
# Usage:
#   perl run.pl [options]
#
# Options:
#   --host    HOST     MariaDB host (default: 127.0.0.1)
#   --port    PORT     MariaDB port (default: 3306)
#   --user    USER     MariaDB user (default: root)
#   --pass    PASS     MariaDB password (default: testpass)
#   --mode    MODE     standalone|sm|mm|all  (default: all)
#   --timeout N        seconds per query (default: 300)
#   --runs    N        timed runs after warmup (default: 3)
#   --output  FORMAT   markdown|csv  (default: markdown)
#   --container NAME   Docker container for standalone DuckDB CLI
#                      (default: duckdb-plugin-dev-ubuntu)
#
# Prerequisite: run setup.sql first.

use strict;
use warnings;
use Getopt::Long;
use Time::HiRes qw(time);
use File::Basename qw(dirname);
use Cwd qw(abs_path);
use DBI;

# ---------------------------------------------------------------------------
# Config / options
# ---------------------------------------------------------------------------

my $PLUGIN_DIR  = abs_path(dirname(__FILE__) . "/../..");
my $QUERY_DIR   = "$PLUGIN_DIR/tests/regression/tpch";
my $DUCKDB_FILE = "/home/shared/duckdb/tpch_perf.duckdb";

my $host        = "127.0.0.1";
my $port        = 3306;
my $user        = "root";
my $pass        = "testpass";
my $mode        = "all";
my $timeout     = 300;
my $runs        = 3;
my $output_fmt  = "markdown";
my $duckdb_bin  = "duckdb";

GetOptions(
    "host=s"      => \$host,
    "port=i"      => \$port,
    "user=s"      => \$user,
    "pass=s"      => \$pass,
    "mode=s"      => \$mode,
    "timeout=i"   => \$timeout,
    "runs=i"      => \$runs,
    "output=s"    => \$output_fmt,
    "duckdb=s"    => \$duckdb_bin,
) or die "Usage: $0 [--host H] [--port P] [--user U] [--pass P] [--mode standalone|sm|mm|all] [--timeout N] [--runs N] [--output markdown|csv] [--duckdb PATH]\n";

my @MODES = ($mode eq "all") ? qw(standalone sm mm) : ($mode);

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

sub info  { print "  \e[33m→\e[0m @_\n" }
sub ok    { print "  \e[32m✓\e[0m @_\n" }
sub fail  { print "  \e[31m✗\e[0m @_\n" }

sub query_files {
    opendir(my $dh, $QUERY_DIR) or die "Cannot open $QUERY_DIR: $!\n";
    my @files = sort grep { /tpch_q\d+\.sql$/ } readdir($dh);
    closedir($dh);
    return map { "$QUERY_DIR/$_" } @files;
}

sub query_label {
    my ($f) = @_;
    return ($f =~ /tpch_q(\d+)/) ? "Q$1" : $f;
}

sub read_query {
    my ($file, $qmode) = @_;
    open(my $fh, '<', $file) or die "Cannot read $file: $!\n";
    my $sql = do { local $/; <$fh> };
    close $fh;
    my $schema = ($qmode eq "mm") ? "tpch_mm" : "tpch_sm";
    $sql =~ s/USE tpch\b/USE $schema/g;
    # Strip the USE statement — DBI selects the database via selectdb
    $sql =~ s/USE \w+\s*;//g;
    return ($sql, $schema);
}

# Connect to MariaDB, return dbh
sub connect_db {
    my ($schema) = @_;
    my $dsn = "DBI:MariaDB:database=$schema;host=$host;port=$port;mariadb_connect_timeout=10";
    my $dbh = DBI->connect($dsn, $user, $pass, {
        RaiseError => 0,
        PrintError => 0,
    }) or die "Cannot connect to MariaDB: " . DBI->errstr . "\n";
    $dbh->{mysql_use_result} = 0;  # fetch all at once
    return $dbh;
}

# Run one query via DBI, return elapsed seconds or undef on error
sub run_via_dbi {
    my ($dbh, $sql) = @_;
    my $start = time();
    my $sth = $dbh->prepare($sql);
    unless ($sth) {
        return (undef, "FAILED: " . $dbh->errstr);
    }
    my $rv = $sth->execute();
    unless ($rv) {
        return (undef, "FAILED: " . $dbh->errstr);
    }
    # Drain results so MariaDB considers the query complete
    $sth->fetchall_arrayref();
    $sth->finish();
    return (time() - $start, "ok");
}

# Run one query via DuckDB CLI (standalone mode, host binary)
sub run_via_duckdb_cli {
    my ($sql, $schema) = @_;
    my $duck_sql = "SET search_path = '$schema'; $sql";
    my $tmpfile  = "/tmp/hd_perf_$$.sql";
    open(my $fh, '>', $tmpfile) or return (undef, "FAILED: cannot write tmp");
    print $fh $duck_sql;
    close $fh;
    my $start = time();
    my $rc = system(
        "timeout $timeout $duckdb_bin -readonly $DUCKDB_FILE " .
        "< $tmpfile > /dev/null 2>&1"
    );
    my $elapsed = time() - $start;
    unlink $tmpfile;
    return $rc == 0 ? ($elapsed, "ok") : (undef, $elapsed >= $timeout ? "TIMEOUT" : "FAILED");
}

# Warmup + N timed runs. Returns (avg_seconds, status_string)
sub benchmark {
    my ($qmode, $sql, $schema, $dbh) = @_;

    my $run_once = sub {
        if ($qmode eq "standalone") {
            return run_via_duckdb_cli($sql, $schema);
        } else {
            return run_via_dbi($dbh, $sql);
        }
    };

    # Warmup
    my (undef, $wstatus) = $run_once->();
    return (undef, $wstatus) unless $wstatus eq "ok";

    # Timed runs
    my @times;
    for (1..$runs) {
        my ($t, $status) = $run_once->();
        return (undef, $status) unless $status eq "ok";
        push @times, $t;
    }

    my $avg = 0; $avg += $_ for @times;
    return ($avg / @times, "ok");
}

sub fmt_time {
    my ($v, $status) = @_;
    return $status unless $status eq "ok";
    return sprintf("%.3f", $v);
}

sub fmt_ratio {
    my ($v, $base) = @_;
    return "-" unless defined $v && defined $base && $base > 0;
    return sprintf("%.2fx", $v / $base);
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

my @files = query_files();
die "No TPC-H query files found in $QUERY_DIR\n" unless @files;

info "HolyDuck TPC-H Performance Harness";
info "MariaDB   : $host:$port";
info "DuckDB    : $duckdb_bin -> $DUCKDB_FILE";
info "Modes     : @MODES";
info "Per query : 1 warmup + $runs timed runs";
info "Timeout   : ${timeout}s";
print "\n";

# Connect once per MariaDB mode (reuse connection across queries)
my %dbh;
for my $qmode (grep { $_ ne "standalone" } @MODES) {
    my $schema = ($qmode eq "mm") ? "tpch_mm" : "tpch_sm";
    $dbh{$qmode} = connect_db($schema);
}

my (@labels, %results);

for my $file (@files) {
    my $label = query_label($file);
    push @labels, $label;

    for my $qmode (@MODES) {
        my ($sql, $schema) = read_query($file, $qmode);
        info "$label / $qmode";

        my ($avg, $status) = benchmark($qmode, $sql, $schema, $dbh{$qmode});
        $results{$label}{$qmode} = { avg => $avg, status => $status };

        if ($status eq "ok") {
            ok sprintf("%-12s %-10s  %.3fs", $label, $qmode, $avg);
        } else {
            fail "$label  $qmode  $status";
        }
    }
    print "\n";
}

$_->disconnect() for values %dbh;

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

my $has_sa = grep { $_ eq "standalone" } @MODES;
my $has_sm = grep { $_ eq "sm"         } @MODES;
my $has_mm = grep { $_ eq "mm"         } @MODES;

print "\n";

if ($output_fmt eq "csv") {
    my @cols = ("Query");
    push @cols, "Standalone_s" if $has_sa;
    push @cols, "SM_s"         if $has_sm;
    push @cols, "MM_s"         if $has_mm;
    push @cols, "SM_overhead"  if $has_sa && $has_sm;
    push @cols, "MM_overhead"  if $has_sa && $has_mm;
    print join(",", @cols) . "\n";

    for my $label (@labels) {
        my $base = $has_sa ? $results{$label}{standalone}{avg} : undef;
        my @row  = ($label);
        push @row, fmt_time($results{$label}{standalone}{avg}, $results{$label}{standalone}{status}) if $has_sa;
        push @row, fmt_time($results{$label}{sm}{avg},         $results{$label}{sm}{status})         if $has_sm;
        push @row, fmt_time($results{$label}{mm}{avg},         $results{$label}{mm}{status})         if $has_mm;
        push @row, fmt_ratio($results{$label}{sm}{avg}, $base) if $has_sa && $has_sm;
        push @row, fmt_ratio($results{$label}{mm}{avg}, $base) if $has_sa && $has_mm;
        print join(",", @row) . "\n";
    }
} else {
    my @cols = ("Query");
    push @cols, "Standalone (s)" if $has_sa;
    push @cols, "SM (s)"         if $has_sm;
    push @cols, "MM (s)"         if $has_mm;
    push @cols, "SM overhead"    if $has_sa && $has_sm;
    push @cols, "MM overhead"    if $has_sa && $has_mm;

    print "| " . join(" | ", @cols)        . " |\n";
    print "| " . join(" | ", map {"---"} @cols) . " |\n";

    for my $label (@labels) {
        my $base = $has_sa ? $results{$label}{standalone}{avg} : undef;
        my @row  = ($label);
        push @row, fmt_time($results{$label}{standalone}{avg}, $results{$label}{standalone}{status}) if $has_sa;
        push @row, fmt_time($results{$label}{sm}{avg},         $results{$label}{sm}{status})         if $has_sm;
        push @row, fmt_time($results{$label}{mm}{avg},         $results{$label}{mm}{status})         if $has_mm;
        push @row, fmt_ratio($results{$label}{sm}{avg}, $base) if $has_sa && $has_sm;
        push @row, fmt_ratio($results{$label}{mm}{avg}, $base) if $has_sa && $has_mm;
        print "| " . join(" | ", @row) . " |\n";
    }
    print "\n";
}
