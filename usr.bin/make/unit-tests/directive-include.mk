# $NetBSD: directive-include.mk,v 1.9 2021/12/14 01:00:04 rillig Exp $
#
# Tests for the .include directive, which includes another file.

# TODO: Implementation

.MAKEFLAGS: -dc

# All included files are recorded in the variable .MAKE.MAKEFILES.
# In this test, only the basenames of the files are compared since
# the directories can differ.
.include "/dev/null"
.if ${.MAKE.MAKEFILES:T} != "${.PARSEFILE} null"
.  error
.endif

# Each file is recorded only once in the variable .MAKE.MAKEFILES.
# Between 2015-11-26 and 2020-10-31, the very last file could be repeated,
# due to an off-by-one bug in ParseTrackInput.
.include "/dev/null"
.if ${.MAKE.MAKEFILES:T} != "${.PARSEFILE} null"
.  error
.endif

.include "nonexistent.mk"
.include "/dev/null"		# size 0
# including a directory technically succeeds, but shouldn't.
#.include "."			# directory

# As of 2020-11-21, anything after the delimiter '"' is ignored.
.include "/dev/null" and ignore anything in the rest of the line.

# The filename to be included can contain expressions.
DEV=	null
.include "/dev/${DEV}"

# Expressions in double quotes or angle quotes are first parsed naively, to
# find the closing '"'.  In a second step, the expressions are expanded.  This
# means that the expressions cannot include the characters '"' or '>'.  This
# restriction is not practically relevant since the expressions inside
# '.include' directives are typically kept as simple as possible.
#
# If the whole line were expanded before parsing, the filename to be included
# would be empty, and the closing '"' would be in the trailing part of the
# line, which is ignored as of 2021-12-03.
DQUOT=	"
.include "${DQUOT}"

# When the expression in a filename cannot be evaluated, the failing
# expression is skipped and the file is included nevertheless.
# FIXME: Add proper error handling, no file must be included here.
.include "nonexistent${:U123:Z}.mk"

# The traditional include directive is seldom used.
include /dev/null		# comment
# expect+1: Cannot open /nonexistent
include /nonexistent		# comment
sinclude /nonexistent		# comment
include ${:U/dev/null}		# comment
include /dev/null /dev/null
# expect+1: Invalid line type
include

# XXX: trailing whitespace in diagnostic, missing quotes around filename
### expect+1: Could not find
# The following include directive behaves differently, depending on whether
# the current file has a slash or is a relative filename.  In the first case,
# make opens the directory of the current file and tries to read from it,
# resulting in the error message """ line 1: Zero byte read from file".
# In the second case, the error message is "Could not find ", without quotes
# or any other indicator for the empty filename at the end of the line.
#include ${:U}

all:
