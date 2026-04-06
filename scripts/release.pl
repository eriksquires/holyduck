#!/usr/bin/env perl
# release.pl — Build HolyDuck for all distros and package release artifacts.
#
# Usage:
#   perl scripts/release.pl <version>
#   perl scripts/release.pl v0.4.2
#
# For each distro:
#   1. Ensures a holyduck-dev-<distro> container exists (creates via
#      recreate_container.pl if missing).
#   2. Builds ha_duckdb.so inside the container (Release mode, no debug).
#   3. Collects the binary into release/<version>/.
#
# Then packages per-distro tarballs with the SQL files.
#
# Only one container can bind port 3306 at a time, so distros are built
# sequentially. The ubuntu container is left running at the end.
use strict;
use warnings;
use File::Basename qw(dirname basename);
use File::Copy     qw(copy);
use File::Path     qw(make_path);
use Cwd            qw(abs_path);

use lib '/home/shared/mariadb/lib';
use MariaDB::Dev           qw(run section fail);
use MariaDB::Dev::Container qw(ensure_running);
use MariaDB::Dev::Project  qw(project_config);

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
my $VERSION = $ARGV[0] or die "Usage: $0 <version>  e.g. $0 v0.4.2\n";

my $ROOT        = abs_path(dirname(abs_path($0)) . "/..");
my @DISTROS     = qw(ubuntu oracle8 oracle9);
my $RELEASE_DIR = "$ROOT/release/$VERSION";

make_path($RELEASE_DIR);

print "HolyDuck release $VERSION\n";
print "Output: $RELEASE_DIR\n\n";

# ---------------------------------------------------------------------------
# Build each distro
# ---------------------------------------------------------------------------
my @built;

for my $distro (@DISTROS) {
    section "BUILD $distro";

    my %cfg = project_config('holyduck', distro => $distro);

    my $CONTAINER    = $cfg{container};
    my $SRC_MNT      = $cfg{container_source_dir};
    my $MDB_MNT      = $cfg{container_mariadb_dir};
    my $build_subdir = $cfg{build_subdir};
    my $BUILD_IN_CTR = "$SRC_MNT/$build_subdir";
    my $SRC_IN_CTR   = "$SRC_MNT/src";

    # Build output on host — release builds go into plugin-out-<distro>
    my $host_build_output = $cfg{build_output};
    my $out_bn    = basename($host_build_output);
    my $OUT_IN_CTR = "$SRC_MNT/$out_bn";

    # Ensure container exists; create in build mode (no MariaDB needed)
    my $exists = `docker ps -a --format '{{.Names}}' 2>/dev/null`;
    if ($exists !~ /^\Q$CONTAINER\E$/m) {
        print "Container $CONTAINER does not exist — creating (build mode)...\n";
        my ($ok, $out) = run(
            "perl /home/shared/mariadb/scripts/recreate_container.pl "
            . "--project=holyduck --distro=$distro --mode=build"
        );
        fail("Failed to create $CONTAINER:\n$out") unless $ok;
    } else {
        # Container exists but may be stopped or another distro may hold the port
        ensure_running($CONTAINER, $cfg{container_prefix});
    }
    print "Container $CONTAINER ready.\n";

    # Ensure host build output dir exists
    system("mkdir", "-p", $host_build_output);

    # Nuke stale cmake cache and configure for Release
    run("docker exec $CONTAINER rm -rf $BUILD_IN_CTR/CMakeCache.txt "
      . "$BUILD_IN_CTR/CMakeFiles");

    my ($cok, $cout) = run(
        "docker exec $CONTAINER bash -c '"
        . "mkdir -p $BUILD_IN_CTR && "
        . "cmake -S $SRC_IN_CTR -B $BUILD_IN_CTR "
        . "  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$OUT_IN_CTR "
        . "  -DMARIADB_SOURCE_DIR=$MDB_MNT "
        . "  -DMARIADB_BUILD_DIR=$MDB_MNT/build "
        . "  -DCMAKE_BUILD_TYPE=Release 2>&1'"
    );
    fail("cmake failed for $distro:\n$cout") unless $cok;

    # Build
    my ($bok, $bout) = run(
        "docker exec $CONTAINER make -j\$(nproc) -C $BUILD_IN_CTR 2>&1"
    );
    my $build_ok = $bok && $bout !~ /\berror:/i;
    for my $line (split /\n/, $bout) {
        print "  $line\n" if $line =~ /Building|Linking|error:|warning:/;
    }
    fail("build failed for $distro:\n$bout") unless $build_ok;

    # Verify .so exists on host
    my $so_host = "$host_build_output/$cfg{plugin_so}";
    fail("$so_host not found after build") unless -f $so_host;

    # Copy to release dir with distro suffix
    my $so_dest = "$RELEASE_DIR/ha_duckdb-$VERSION-$distro.so";
    copy($so_host, $so_dest) or fail("copy to release dir failed: $!");
    my $size_mb = sprintf("%.1f", (-s $so_dest) / 1024 / 1024);
    print "  Built: $so_dest (${size_mb}MB)\n";

    push @built, $so_dest;
}

# ---------------------------------------------------------------------------
# Copy SQL files (distro-independent)
# ---------------------------------------------------------------------------
section "SQL FILES";

for my $sql (glob("$ROOT/sql/holyduck_*.sql")) {
    my $bn = basename($sql);
    copy($sql, "$RELEASE_DIR/$bn") or warn "copy $bn failed: $!\n";
    print "  $bn\n";
}

# ---------------------------------------------------------------------------
# Package per-distro tarballs
# ---------------------------------------------------------------------------
section "TARBALLS";

for my $distro (@DISTROS) {
    my $tarball = "ha_duckdb-$VERSION-$distro.tar.gz";
    my @files = (
        "ha_duckdb-$VERSION-$distro.so",
        glob("$RELEASE_DIR/holyduck_*.sql"),
    );
    # Map to basenames for tar
    my @basenames = ("ha_duckdb-$VERSION-$distro.so");
    push @basenames, map { basename($_) } glob("$RELEASE_DIR/holyduck_*.sql");

    my ($ok, $out) = run(
        "tar -czf $RELEASE_DIR/$tarball -C $RELEASE_DIR "
        . join(' ', map { "'$_'" } @basenames)
    );
    fail("tar failed for $distro:\n$out") unless $ok;
    my $size = sprintf("%.1f", (-s "$RELEASE_DIR/$tarball") / 1024 / 1024);
    print "  $tarball (${size}MB)\n";
}

# ---------------------------------------------------------------------------
# Restore ubuntu as the running container
# ---------------------------------------------------------------------------
section "RESTORE";
ensure_running("holyduck-dev-ubuntu", "holyduck-dev");
print "holyduck-dev-ubuntu running.\n";

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print "\nRelease $VERSION complete.\n\n";
print "Artifacts:\n";
for my $f (sort glob("$RELEASE_DIR/*")) {
    printf "  %-50s %s\n", basename($f), sprintf("%.1fMB", (-s $f) / 1024 / 1024);
}
