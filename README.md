# STATUS

Currently working!

Well, the tests, at least.

# Introduction

This is a FUSE-based filesystem that implements Copy-on-Write semantics over an existing directory structure.

What you do is use the `cow_fuse` binary to mount the filesystem atop a directory structure that already
exists. `cow_fuse` then replaces that filesystem with an identical one. Any changes made to that filesystem,
be it the contents modified, file modes changed, files renamed, or new files added are recorded, with the original
directory structure still accessible.

This software is still in development, so not only do I think there's bugs, I can nearly assure you of some.

# Requirements

* Your compiler must support C++11, gcc 4.9 is sufficient, and probably some older versions will
work too.

* sqlite3 is required, as it is used to store history data.

* Linux; no other OS is supported, although porting to any Unix-like OS with FUSE support
should be something between trivial and easy.

# Usage

So let's say you have a collection of files in a directory called "data"; we can create a COW-version of
data called "data2":

## Mounting

	mkdir data2
	cow_fuse data data2

Now, `data2` looks exactly like `data`. (`data` will get the directory `.cow` which stores history information).

Any changes made to `data2` immediately get applied to `data`; you should not modify `data`, but you can read from it
without any of the performance penalties associated with FUSE.

## Features

The directory `data2` works like any directory, except it's slower and has the COW feature. Inside `data2` is
another directory named `.original` which doesn't get listed, even with `ls -a` (so that `cp -a` doesn't
copy it). The directory `.original` contains all files as they were before any changes were made to `data2`.

# Future Plans

* My intent is that the command `cow_fuse data` replaces the directory `data` with itself, only with the COW feature. This 
is nearly done, it's mostly just UI that is missing.

* The GNU command `cp` has the option `--reflink=always` which is used for making
Copy-On-Write copies of whole files; this should be supported.

* It'd be nice to provide a "snapshot" command to allow you to store any number of states
of the directory structure.

# Regression Testing

Automated tests are included, they check various aspects of the software's stability. I
generally update them as I add new features, and I generally run the tests very frequently
and before committing.

# Bugs

* Buggy in general. Don't trust it yet!

* Since Linux provides no facility for filesystem atomicity, failures can cause corruptions
in the snapshots, since a modification to the current file may be written to disk
before the history file is written. If a system failure occurs between these two events,
then the snapshot will be wrong. Linux doesn't really ensure an order of writes which is highly
unfortunate. The only way to fix this is for the current version to be stored in a special
format and the history to remain as real filesystem entries.

