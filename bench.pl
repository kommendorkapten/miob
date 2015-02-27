#!/usr/bin/env perl

#
# Fredrik Skogman 2015, skogman@gmail.com
#

use strict;
use warnings;

my $cmd = $ARGV[0];
my @steps = qw(100 200 300 400 500 1000 3000 5000 10000 15000 20000 15000 30000);
my $count = 10;
my $os = `uname`;
my %result;
my %opts = (
    'Darwin' => [qw(select poll kqueue)],
    'SunOS'  => [qw(select poll port)],
    'Linux'  => [qw(select poll epoll)],
);

chomp $os;

foreach my $step (@steps) {
    foreach my $method (@{$opts{$os}}) {
	my $acc = b_method($step, $method);
	if ($acc < 0) {
	    next;
	}
	$result{$method}{$step} = $acc;
    }
}

print "Method ";
foreach my $step (@steps) {
    if ($step < 1000) {
	printf "%4d", $step;
    } else {
	printf "%6d", $step;
    }
}
print "\n";

foreach my $method (keys %result) {
    my $res = $result{$method};
    printf "%-6s ", $method;
    foreach my $step (@steps) {
	my $avg = '-';
	if (defined $$res{$step}) {
	    $avg = int ($$res{$step} / 10);
	}
	if ($step < 1000) {
	    printf "%4s", $avg;
	} else {
	    printf "%6s", $avg;
	}
    }
    print "\n";
}

sub b_method
{
    my ($fds, $method) = @_;
    my $i = 0;
    my $acc = 0;

    while ($i++ < $count) {
	my $out = `$cmd -f $fds -m $method`;
	if ($? != 0) {
	    print "[$method]: Failed at $fds number of fds\n";
	    return -1;
	}
	if ($out =~ /was (\d+)us/) {
	    $acc += $1;
	} else {
	    return -1;
	}

    }
    return $acc;
}
