# mkblob
Make library independent executable files.

Make with makeall_el8.sh on x86_64 version of CentOS 8 or Red Hat Enterprise 8.
Precompiled binary is included in directory binary_x86_64.

mkblob should run on any x86_64 as it is statically compiled
and includes all files it needs.

With mkblob you can make new library independet executables which you can move
around different Linux distributions without having to worry about dependent 
shared object files or other dependent files.

Example: mkblob /usr/bin/ls -o ls.blob -static

License is for now GPLv3.
