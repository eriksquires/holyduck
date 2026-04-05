#!/usr/bin/env perl
# HolyDuck build-and-test wrapper.
#
# HolyDuck builds in-container (links against the container's MariaDB
# headers and needs a glibc/ABI matching libduckdb.so v1.5.0), so this
# wrapper does the BUILD inside the container and then execs into the
# shared /home/shared/mariadb/scripts/build_and_test.pl with --skip-build
# for the DEPLOY / SMOKE / REGRESSION phases.
#
# Usage: perl scripts/build_and_test.pl [--distro=ubuntu|oracle8|oracle9|debian12]
#                                       [--no-test]
#
# All flags except --distro are forwarded verbatim to the shared script.
use strict;
use warnings;
use File::Basename qw(basename);

use lib '/home/shared/mariadb/lib';
use MariaDB::Dev           qw(run section fail);
use MariaDB::Dev::DB       qw(wait_for_ready);
use MariaDB::Dev::Container qw(ensure_running);
use MariaDB::Dev::Project  qw(project_config);

# ---------------------------------------------------------------------------
# Args — --distro is consumed here; everything else is passed through
# ---------------------------------------------------------------------------
my $DISTRO    = 'ubuntu';
my @passthru;
for my $arg (@ARGV) {
    if ($arg =~ /^--distro=(.+)/) { $DISTRO = $1; }
    else                          { push @passthru, $arg; }
}

my %cfg = project_config('holyduck', distro => $DISTRO);

my $CONTAINER = $cfg{container};
my $SRC_MNT   = $cfg{container_source_dir};   # /project-src
my $MDB_MNT   = $cfg{container_mariadb_dir};  # /mariadb-src

# Container-side view of $cfg{build_output} (bind-mounted under source_dir).
my $out_bn      = basename($cfg{build_output});      # plugin-out-<distro>
my $OUT_IN_CTR  = "$SRC_MNT/$out_bn";
my $BUILD_IN_CTR= "$SRC_MNT/$cfg{build_subdir}";     # /project-src/build
my $SRC_IN_CTR  = "$SRC_MNT/src";                    # CMakeLists.txt dir

my %db_opts     = (host => $cfg{db_host}, port => $cfg{db_port});

# ---------------------------------------------------------------------------
# 1. Ensure container is up (build runs inside it)
# ---------------------------------------------------------------------------
section "BUILD (in-container, HolyDuck)";
my $was_running = ensure_running($CONTAINER, $cfg{container_prefix});
wait_for_ready(%db_opts, container => $CONTAINER, start_service => 1)
    unless $was_running;

# Make sure the host-side build_output dir exists — bind-mount isn't
# needed here, it's a plain subdir of source_dir and thus already visible
# in the container under /project-src.
system("mkdir", "-p", $cfg{build_output});

# ---------------------------------------------------------------------------
# 2. Detect stale CMakeCache.txt and reconfigure if needed
# ---------------------------------------------------------------------------
my $host_cache   = "$cfg{source_dir}/$cfg{build_subdir}/CMakeCache.txt";
my ($configured_src, $configured_out) = ('', '');
if (open my $cf, '<', $host_cache) {
    while (<$cf>) {
        $configured_src = $1 if /^CMAKE_HOME_DIRECTORY[^=]*=(.+)/;
        $configured_out = $1 if /CMAKE_LIBRARY_OUTPUT_DIRECTORY[^=]*=(.+)/;
    }
    close $cf;
}
chomp $configured_src; chomp $configured_out;

my $need_reconfigure = ($configured_src ne $SRC_IN_CTR)
                    || ($configured_out ne $OUT_IN_CTR);

if ($need_reconfigure) {
    print "Reconfiguring cmake (src='$configured_src' want='$SRC_IN_CTR'; "
        . "out='$configured_out' want='$OUT_IN_CTR')\n";
    # Cache + CMakeFiles are root-owned (written from inside the container),
    # so nuke them via docker exec.
    run("docker exec $CONTAINER rm -rf $BUILD_IN_CTR/CMakeCache.txt "
      . "$BUILD_IN_CTR/CMakeFiles");

    my ($ok, $out) = run(
        "docker exec $CONTAINER bash -c '"
        . "mkdir -p $BUILD_IN_CTR && "
        . "cmake -S $SRC_IN_CTR -B $BUILD_IN_CTR "
        . "  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$OUT_IN_CTR "
        . "  -DMARIADB_SOURCE_DIR=$MDB_MNT "
        . "  -DMARIADB_BUILD_DIR=$MDB_MNT/build "
        . "  -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1'"
    );
    fail("cmake reconfigure failed:\n$out") unless $ok;
}

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------
# Force recompile by removing the object file so the timestamp is fresh.
run("docker exec $CONTAINER rm -f "
  . "$BUILD_IN_CTR/CMakeFiles/$cfg{soname}.dir/src/$cfg{soname}.cc.o");

my ($bok, $bout) = run(
    "docker exec $CONTAINER make -j\$(nproc) -C $BUILD_IN_CTR"
);
my $build_ok = $bok && $bout !~ /\berror:/i;
for my $line (split /\n/, $bout) {
    print "$line\n" if $line =~ /Building|Linking|error:|warning:/;
}
fail("in-container build failed:\n$bout") unless $build_ok;

fail(".so not found after build: $cfg{build_output}/$cfg{plugin_so}")
    unless -f "$cfg{build_output}/$cfg{plugin_so}";

print "Built: $cfg{build_output}/$cfg{plugin_so}\n";

# ---------------------------------------------------------------------------
# 4. Clear any root-owned plugin files from legacy in-container builds so
#    the shared script's host-side copy() in DEPLOY doesn't hit EACCES.
# ---------------------------------------------------------------------------
run("docker exec $CONTAINER rm -f "
  . "$cfg{container_plugin_dir}/$cfg{plugin_so}");

# ---------------------------------------------------------------------------
# 5. Hand off to shared script for DEPLOY + SMOKE + REGRESSION
# ---------------------------------------------------------------------------
exec("/home/shared/mariadb/scripts/build_and_test.pl",
     "--project=holyduck", "--distro=$DISTRO", "--skip-build", @passthru)
    or fail("exec into shared build_and_test.pl failed: $!");
