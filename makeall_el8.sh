#!/bin/bash
# Script to make mkblob
# (c) 2020 Sigurd Dagestad 

# Remove old blob files
rm -rf blob
rm -rf blob.tar.gz
rm -rf cde

# Unpack CDE
tar -zxf CDE.tar.gz

# Make cde as static
cd ./CDE/readelf-mini
make
cd ..
gcc -Wall -g -O2 -D_GNU_SOURCE -DOKAPI_STANDALONE strace-4.6/okapi.c -o okapi -static
strip okapi
mv okapi ..
cd ./strace-4.6/ #configure dir
./configure LDFLAGS="-static"
#cp ../cdefixes/Makefile.CDEstraceStatic ./strace-4.6/Makefile
cp ../cdefixes/fakeokapiconfig.h ./strace-4.6/config.h
make 
cd ../..
cp ./CDE/strace-4.6/cde .


# Make and copy files for building system 
mkdir blob
cp cde ./blob
cp okapi ./blob
cp mkblob.c ./blob
cp mkblobexec.c ./blob
cp blobgz.S ./blob
strip ./blob/cde
strip ./blob/okapi

cd ./blob

# There will be error messages here as this only builds 
# the new "fake" compiler environment.
echo -e "\e[5m\e[92m\"Fake\" compiling to gather files using bogus tull.c, there will be error messages, just ignore!\e[25m"
./cde /usr/bin/gcc tull.c  
cp ../templates/* ./cde-package
./cde /usr/libexec/gcc/x86_64-redhat-linux/8/cc1 tull.c 
./cde /usr/bin/as tull.c 
./cde /usr/libexec/gcc/x86_64-redhat-linux/8/collect2 tull.c 
./cde /usr/bin/ld tull.c 
./cde gcc -c mkblobexec.c blobgz.S -o mkblobexec.o
./cde gcc mkblob.c -o mkblob -lpthread -static 
#./cde gcc -c mkblobexec.c blobgz.S -o mkblobexec.o
# Make both static and non static as we need both since 
# we are able to choose what we want.
# -lpthread and -static make sure libpthread.a and libc.a
# are included for threads and static building. 
./cde gcc mkblob.c -o mkblob
./cde gcc mkblob.c -o mkblob -lpthread
./cde gcc mkblob.c -o mkblob -lpthread -static 

# Remove home directory.
rm -rf ./cde-package/cde-root/home

# Move folder and files down
cd ./cde-package
mv * ..
#mv *.* ..
cd ..
rm -rf ./cde-package


# Make files ready for mkblob system
cd ..
cp mkblobexec.c mkblobexec_dae.c
sed -i 's/deleteAtExit=0/deleteAtExit=1/g' mkblobexec_dae.c
gcc -c mkblobexec_dae.c -o mkblobexec_dae.o
rm -rf mkblobexec_dae.c
gcc -c mkblobexec.c -o mkblobexec.o
mv mkblobexec_dae.o ./blob/mkblobexec_dae.o
mv mkblobexec.o ./blob/mkblobexec.o

# Remove c files from blob.
rm -rf ./blob/*.c 
rm -rf ./blob/cde.uname
rm -rf ./blob/cde.log

# Remove big local-archive from blob
rm -rf   blob/cde-root/usr/lib/locale/

# zip and make mkblob
tar -zcvf blob.tar.gz blob/
gcc mkblob.c  blobgz.S -o mkblob -static
strip mkblob
echo "ldd mkblob"
ldd mkblob
echo "Done."
 

#rm -rf mkblob.c mkblob mkblobexec.c

