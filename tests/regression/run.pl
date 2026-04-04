#!/usr/bin/env perl
# HolyDuck regression test runner.
# Usage: perl run.pl [--update] [--host=HOST] [--port=PORT]
#
# Runs all *.sql tests in this directory and the tpch/ subdirectory.
# Each test resets the database to a clean state via setup.sql before running.
#
# Host and port default to the holyduck project config from projects.yaml.

use strict;
use warnings;
use DBI;
use Try::Tiny;
use File::Basename qw(dirname basename);
use Cwd            qw(abs_path);

use lib '/home/shared/mariadb/lib';
use MariaDB::Dev::Project qw(project_config);

my %cfg = project_config('holyduck');

my $DIR    = abs_path(dirname(abs_path($0)));
my $UPDATE = 0;
my $HOST   = $cfg{db_host};
my $PORT   = $cfg{db_port};

for my $arg (@ARGV) {
    $UPDATE = 1    if $arg eq '--update';
    $HOST   = $1   if $arg =~ /^--host=(.+)/;
    $PORT   = $1   if $arg =~ /^--port=(\d+)/;
}

# ---------------------------------------------------------------------------
# DBI
# ---------------------------------------------------------------------------

my $dbh = DBI->connect(
    "DBI:MariaDB:host=$HOST;port=$PORT", 'root', 'testpass',
    { RaiseError => 0, PrintError => 0 }
) || DBI->connect(
    "DBI:MariaDB:host=$HOST;port=$PORT", 'root', '',
    { RaiseError => 1, PrintError => 0 }
) or die "Cannot connect to MariaDB at $HOST:$PORT: " . DBI->errstr . "\n";

sub run_sql_file {
    my ($sql_file) = @_;

    open my $fh, '<', $sql_file
        or do { warn "Cannot open $sql_file: $!\n"; return ("ERROR: $!") };
    my $sql = do { local $/; <$fh> };
    close $fh;

    $sql =~ s/--[^\n]*//g;
    my @stmts = grep { /\S/ } split /;/, $sql;
    my @output;

    for my $stmt (@stmts) {
        $stmt =~ s/^\s+|\s+$//g;
        next unless $stmt;

        try {
            if ($stmt =~ /^USE\s+/i) {
                $dbh->do($stmt);
            } elsif ($stmt =~ /^(?:SELECT|SHOW|EXPLAIN|DESCRIBE|WITH)\b/i) {
                my $sth = $dbh->prepare($stmt);
                $sth->execute();
                push @output, join("\t", @{$sth->{NAME}});
                while (my $row = $sth->fetchrow_arrayref) {
                    push @output, join("\t", map { defined $_ ? $_ : 'NULL' } @$row);
                }
                $sth->finish();
            } else {
                $dbh->do($stmt);
            }
        } catch {
            my $err = $_;
            push @output, "ERROR: $err";
        };
    }

    return @output;
}

# ANSI colours
sub pass { printf "  \e[32mPASS\e[0m  %s\n", $_[0] }
sub fail { printf "  \e[31mFAIL\e[0m  %s\n", $_[0] }
sub info { printf "  \e[33m----\e[0m  %s\n", $_[0] }

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

info "Setting up regression database...";
run_sql_file("$DIR/setup.sql");

# Also run tpch setup if present
run_sql_file("$DIR/tpch/setup.sql") if -f "$DIR/tpch/setup.sql";
print "\n";

# ---------------------------------------------------------------------------
# Collect tests: main directory first, then tpch/ subdirectory
# ---------------------------------------------------------------------------

my @sql_files;
push @sql_files, sort grep { !/setup\.sql|teardown\.sql/ } glob("$DIR/*.sql");
push @sql_files, sort grep { !/setup\.sql/ } glob("$DIR/tpch/*.sql");

my ($pass_count, $fail_count, $new_count) = (0, 0, 0);

for my $sql_file (@sql_files) {
    my $name     = basename($sql_file, '.sql');
    my $test_dir = dirname($sql_file);
    my $expected = "$test_dir/$name.expected";

    my @lines  = run_sql_file($sql_file);
    my $actual = join("\n", @lines);
    chomp $actual;

    if ($UPDATE) {
        open my $fh, '>', $expected or die "Cannot write $expected: $!";
        print $fh "$actual\n";
        close $fh;
        info "$name  (updated)";
        ++$new_count;
        next;
    }

    if (!-f $expected) {
        open my $fh, '>', $expected or die "Cannot write $expected: $!";
        print $fh "$actual\n";
        close $fh;
        info "$name  (no .expected — created from current output)";
        ++$new_count;
        next;
    }

    open my $fh, '<', $expected or die "Cannot read $expected: $!";
    my $want = do { local $/; <$fh> };
    close $fh;
    chomp $want;

    if ($actual eq $want) {
        pass $name;
        ++$pass_count;
    } else {
        fail $name;
        my @want_lines = split /\n/, $want;
        my @got_lines  = split /\n/, $actual;
        print "    Expected:\n";
        print "      $_\n" for @want_lines;
        print "    Got:\n";
        print "      $_\n" for @got_lines;
        ++$fail_count;
    }
}

# ---------------------------------------------------------------------------
# Teardown
# ---------------------------------------------------------------------------
print "\n";
info "Tearing down regression database...";
run_sql_file("$DIR/teardown.sql");

$dbh->disconnect;

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print "\n";
if ($UPDATE) {
    printf "\e[33mUpdated %d expected file(s).\e[0m\n", $new_count;
} elsif ($fail_count == 0) {
    printf "\e[32mAll tests passed (%d passed, %d new).\e[0m\n",
        $pass_count, $new_count;
    exit 0;
} else {
    printf "\e[31m%d test(s) failed, %d passed, %d new.\e[0m\n",
        $fail_count, $pass_count, $new_count;
    exit 1;
}
