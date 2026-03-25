#!/usr/bin/env perl
# build-all.pl — Build ha_duckdb for all supported distros and collect binaries.
#
# Usage:
#   ./scripts/build-all.pl [output-dir]
#
# Builds ubuntu, oracle8, oracle9 in sequence, stopping/starting containers
# as needed (only one can bind port 3306 at a time). Copies each binary to
# output-dir (default: ./release-binaries) with a distro-suffixed name.
#
# Prerequisite: cmake-setup.sh must have been run once per container.
# MARIADB_SRC_DIR must be set in the environment.

use strict;
use warnings;
use File::Basename qw(dirname);
use File::Path qw(make_path);
use Cwd qw(abs_path);

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

my $PLUGIN_DIR  = abs_path(dirname(__FILE__) . "/..");
my $OUTPUT_RAW  = $ARGV[0] // "$PLUGIN_DIR/release-binaries";
make_path($OUTPUT_RAW);
my $OUTPUT_DIR  = abs_path($OUTPUT_RAW);

my @DISTROS = (
    { name => "ubuntu",  container => "duckdb-plugin-dev-ubuntu",  build_subdir => "build",         mariadb_build => "build"        },
    { name => "oracle8", container => "duckdb-plugin-dev-oracle8",  build_subdir => "build-oracle8", mariadb_build => "build" },
    { name => "oracle9", container => "duckdb-plugin-dev-oracle9",  build_subdir => "build-oracle9", mariadb_build => "build" },
);

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

sub info  { print "  \e[33m→\e[0m @_\n" }
sub ok    { print "  \e[32m✓\e[0m @_\n" }
sub error { print "  \e[31m✗\e[0m @_\n" }

sub run {
    my ($cmd) = @_;
    system($cmd) == 0 or die "Command failed: $cmd\n";
}

sub run_or_warn {
    my ($cmd) = @_;
    my $rc = system($cmd);
    return $rc == 0;
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

die "MARIADB_SRC_DIR is not set\n" unless $ENV{MARIADB_SRC_DIR};

make_path($OUTPUT_DIR);
info "Output directory: $OUTPUT_DIR";
print "\n";

my @built;

for my $d (@DISTROS) {
    my $name         = $d->{name};
    my $container    = $d->{container};
    my $build_subdir = $d->{build_subdir};
    my $mariadb_build= $d->{mariadb_build};

    info "=== Building $name ===";

    # docker-run.sh stops other containers and starts this one
    info "Starting container $container...";
    run("MARIADB_SRC_DIR=$ENV{MARIADB_SRC_DIR} $PLUGIN_DIR/scripts/docker-run.sh $name");

    # Wait for MariaDB to be ready before building
    info "Waiting for MariaDB to be ready...";
    my $ready = 0;
    for (1..30) {
        my $rc = system("docker exec $container mysqladmin -uroot -ptestpass --ssl=0 ping --silent 2>/dev/null");
        if ($rc == 0) { $ready = 1; last; }
        sleep 1;
    }
    die "MariaDB in $container did not start within 30 seconds\n" unless $ready;
    ok "MariaDB is ready.";

    # Configure for Release (no debug symbols) with output into build subdir
    info "Configuring cmake (Release)...";
    run(qq{docker exec $container bash -c "
        mkdir -p /plugin-src/$build_subdir && \\
        cmake /plugin-src/src \\
          -B /plugin-src/$build_subdir \\
          -DMARIADB_SOURCE_DIR=/mariadb-src \\
          -DMARIADB_BUILD_DIR=/mariadb-src/$mariadb_build \\
          -DCMAKE_BUILD_TYPE=Release \\
          -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=/plugin-src/$build_subdir \\
          2>&1 | tail -3
    "});

    # Build
    info "Building...";
    run(qq{docker exec $container bash -c "cmake --build /plugin-src/$build_subdir -j\$(nproc) 2>&1 | tail -3"});

    # Copy binary — cmake outputs ha_duckdb.so (no lib prefix, see CMakeLists.txt PREFIX "")
    my $binary = "$PLUGIN_DIR/$build_subdir/ha_duckdb.so";
    die "Build failed — $binary not found\n" unless -f $binary;
    my $dest = "$OUTPUT_DIR/ha_duckdb-$name.so";
    run("cp $binary $dest");
    my $size = -s $dest;
    ok "Built $dest (" . int($size / 1024 / 1024) . "MB)";

    push @built, $dest;
    print "\n";
}

# Copy SQL files — same for all distros, copy once
for my $sql_file (qw(holyduck_duckdb_extensions.sql holyduck_mariadb_functions.sql)) {
    my $src = "$PLUGIN_DIR/sql/$sql_file";
    die "SQL file not found: $src\n" unless -f $src;
    run("cp $src $OUTPUT_DIR/$sql_file");
    ok "Copied $sql_file";
}

# Restore ubuntu as the default active container
info "Restoring ubuntu container as active...";
run_or_warn("MARIADB_SRC_DIR=$ENV{MARIADB_SRC_DIR} $PLUGIN_DIR/scripts/docker-run.sh ubuntu");

print "\n";
ok "All builds complete.";
print "\n  Binaries:\n";
print "    $_\n" for @built;
print "\n  SQL files:\n";
print "    $OUTPUT_DIR/$_\n" for qw(holyduck_duckdb_extensions.sql holyduck_mariadb_functions.sql);
