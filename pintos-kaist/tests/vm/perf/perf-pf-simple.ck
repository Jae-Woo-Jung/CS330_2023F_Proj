# -*- perl -*-
use strict;
use warnings;
use tests::tests;
our ($test);
my (@output) = read_text_file ("$test.output");

common_checks ("run", @output);

fail "missing 'test done' message\n"
  if !grep ($_ eq '(perf-pf-simple) test done', @output);
pass;