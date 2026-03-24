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

my $PLUGIN_DIR = abs_path(dirname(__FILE__) . "/..");
my $OUTPUT_DIR = abs_path($ARGV[0] // "$PLUGIN_DIR/release-binaries");

my @DISTROS = (
    { name => "ubuntu",  container => "duckdb-plugin-dev-ubuntu",  binary => "build/libha_duckdb.so"         },
    { name => "oracle8", container => "duckdb-plugin-dev-oracle8",  binary => "build-oracle8/libha_duckdb.so" },
    { name => "oracle9", container => "duckdb-plugin-dev-oracle9",  binary => "build-oracle9/libha_duckdb.so" },
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
    my $name      = $d->{name};
    my $container = $d->{container};
    my $binary    = "$PLUGIN_DIR/$d->{binary}";

    info "=== Building $name ===";

    # docker-run.sh stops other containers and starts this one
    info "Starting container $container...";
    run("MARIADB_SRC_DIR=$ENV{MARIADB_SRC_DIR} $PLUGIN_DIR/scripts/docker-run.sh $name");

    # Wait for MariaDB to be ready before deploying
    info "Waiting for MariaDB to be ready...";
    my $ready = 0;
    for (1..30) {
        my $rc = system("docker exec $container mysqladmin -uroot -ptestpass --ssl=0 ping --silent 2>/dev/null");
        if ($rc == 0) { $ready = 1; last; }
        sleep 1;
    }
    die "MariaDB in $container did not start within 30 seconds\n" unless $ready;
    ok "MariaDB is ready.";

    # Build and deploy
    info "Building and deploying...";
    run("$PLUGIN_DIR/scripts/deploy.sh $container");

    # Copy binary
    my $dest = "$OUTPUT_DIR/ha_duckdb-$name.so";
    run("cp $binary $dest");
    my $size = -s $dest;
    ok "Built $dest (" . int($size / 1024 / 1024) . "MB)";

    push @built, $dest;
    print "\n";
}

# Restore ubuntu as the default active container
info "Restoring ubuntu container as active...";
run_or_warn("MARIADB_SRC_DIR=$ENV{MARIADB_SRC_DIR} $PLUGIN_DIR/scripts/docker-run.sh ubuntu");

print "\n";
ok "All builds complete. Binaries:";
print "    $_\n" for @built;
