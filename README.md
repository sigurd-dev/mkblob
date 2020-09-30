# mkblob
Tool to make executables independent from libraries.

Make with makeall_el8.sh on x86_64 version of CentOS 8 or Red Hat Enterprise 8. 

You might have to tweak the script to make it compile on other distributions.
Also glibc-static is needed to compile. 
Precompiled binary is included in directory binary_x86_64 as well as rpm and deb files.

mkblob should run on any X86_64 as it is statically compiled
and includes all files it needs.

With mkblob you can make new library independent executables which you can move
around different Linux distributions. It will gather all the dependecies 
it needs to run and other files your program uses.

Project was originally started to make opencv programs able to run without recompiling/rebuilding/installing.

Example: mkblob /usr/bin/ls -o ls.blob -static

mkblob can be used on not only binaries, but scripts as well, like shell, perl and python and so on.
If you need or want to pass arguments to your program you should create a script which does this and then run 
this script with mkblob.

For more information please visit: <a href=http://www.dagestad.info/mkblob>http://www.dagestad.info/mkblob</a> 

License is for now GPLv3.
