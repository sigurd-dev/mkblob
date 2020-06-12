/*
mkblob.c
(C) Sigurd Dagestad, Feb. 2020
sigurd@dagestad.info
*/

//Size and content of tar blob
extern const unsigned int size_tar;
extern const char blob_tar[];


#include <stdlib.h>

#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <time.h>

#include <limits.h>

//#include <dirent.h>
#include <errno.h>

//chmod
#include <sys/stat.h>

#include <dirent.h>

/* alphabet: [a-z0-9] */
const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";

/**
 * not a cryptographically secure number
 * return interger [0, n).
 */
int intN(int n) { return rand() % n; }

/**
 * Input: length of the random string [a-z0-9] to be generated
 */
char *randomString(int len) 
{
  char *rstr = malloc((len + 1) * sizeof(char));
  int i;
  for (i = 0; i < len; i++) 
  {
    rstr[i] = alphabet[intN(strlen(alphabet))];
  }
  rstr[len] = '\0';
  return rstr;
}

static void list_dir (const char * dir_name)
{
    DIR * d;
    char cpyfile[256];
 
    /* Open the directory specified by "dir_name". */
    d = opendir (dir_name);
    //printf("opendir: %s\n", opendir);    

    /* Check it was opened. */
    if (! d) {
        fprintf (stderr, "Cannot open directory '%s': %s\n",
                 dir_name, strerror (errno));
        exit (EXIT_FAILURE);
    }
    while (1) {
        struct dirent * entry;
        const char * d_name;

        /* "Readdir" gets subsequent entries from "d". */
        entry = readdir (d);
        if (! entry) {
            /* There are no more entries in this directory, so break
               out of the while loop. */
            break;
        }
        d_name = entry->d_name;
        /* Print the name of the file and directory. */
        if (strcmp (d_name, "..") != 0 &&
            strcmp (d_name, ".") != 0) //strcmp (d_name, entry->d_name) != 0)
             
        {
           //okapi filecopy
           strcpy(cpyfile, "okapi \"");  
           strcat(cpyfile, dir_name);
           strcat(cpyfile, "/");
           strcat(cpyfile, d_name);
           strcat(cpyfile, "\" \"\" ./cde-package/cde-root/");
           system(cpyfile);
       } 
 

        if (entry->d_type & DT_DIR) {

            /* Check that the directory is not "d" or d's parent. */
             if (strcmp (d_name, "..") != 0 &&
                strcmp (d_name, ".") != 0) {
                int path_length;
                char path[PATH_MAX];
 
                path_length = snprintf (path, PATH_MAX,
                                        "%s/%s", dir_name, d_name);
                //printf ("%s\n", path);
                if (path_length >= PATH_MAX) {
                    fprintf (stderr, "Path length has got too long.\n");
                    exit (EXIT_FAILURE);
                }
                /* Recursively call "list_dir" with the new path. */
                list_dir (path);
            }
	}
    }
    /* After going through all the entries, close the directory. */
    if (closedir (d)) {
        fprintf (stderr, "Could not close '%s': %s\n",
                 dir_name, strerror (errno));
        exit (EXIT_FAILURE);
    }
}


char startdir[PATH_MAX];

int main(int argc, char *argv[])
{

   if (argc < 3)
   {
      fprintf(stderr, "Usage: %s <full path to executable file> -o <output blob> [-f file] [-d dir] [-static] [-dae]\n\n", argv[0]);
      fprintf(stderr, "where full path to executable file, executable file you want to make library independent.\n");
      fprintf(stderr, "      -o output file or blob as in binary large object, or as The Blob on the silverscreen.\n");
      fprintf(stderr, "      -f file to include, use multiple if nessesary.\n");
      fprintf(stderr, "      -d dir to include, use multiple if nessesary.\n");
      fprintf(stderr, "      -static, make a static executable for use on compatible arch.\n");
      fprintf(stderr, "      -dae, delete at end, delete files from /tmp/.<randomdir> when finish, to be used if program needs to load files at runtime.\n\n");
      fprintf(stderr, "Example: mkblob /usr/bin/ls -o ls.blob -static\n\n");
      fprintf(stderr, "Project was originally started to make opencv programs able to run without recompiling/rebuilding/installing.\n\n");
      fprintf(stderr, "2020 © Sigurd Dagestad (sigurd@dagestad.info). Version 1.03\n\n");
      return 1;
   } 

   //default output filename
   char outfile[256] = "a.out";

   char gccstatic[20] = "";
   char gccpthread[20] = " -lpthread";

   char cwd[PATH_MAX];
   readlink("/proc/self/cwd", cwd, sizeof(cwd));
   readlink("/proc/self/cwd", startdir, sizeof(startdir));
   strcat(startdir,"/");

   //Make random directory
   srand(time(NULL));
   char p[11];
   strcpy(p, ".");
   strcat(p, randomString(10));

   //Write tar blob to /tmp 
   FILE *pFile;
   char blobtar[256];
   strcpy(blobtar, "/tmp/.blob");
   strcat(blobtar,p);
   strcat(blobtar,".tar.gz");
   pFile = fopen(blobtar,"wb");  // w for wr
   //Write file, this seems to work
   fwrite(blob_tar,size_tar,1,pFile);
   fclose(pFile);
  
   chdir("/tmp/");;
   mkdir(p, 0755);
   char tarcommand[256];

   //tar -xf blob.tar -C 1234 --strip-components=1
   strcpy(tarcommand, "tar -zxf ");
   strcat(tarcommand, blobtar);
   strcat(tarcommand, " -C ");
   strcat(tarcommand, p); 
   strcat(tarcommand, " --strip-components=1");
   system(tarcommand); 

   remove(blobtar);

   char filename[PATH_MAX];
   char dir[PATH_MAX];

   //Max argument string to 8192 bytes. Enough?
   char export[256];
   strcpy(export, "export PATH=/tmp/");
   strcat(export, p);
   strcat(export, ":");
   char command[8192];
   strcpy(command, export);
   
   const char *envpath = "PATH";
   char * pTmp = getenv( envpath );
   
   strcat(command, pTmp);
   strcat(command,";cde ");
 
   //Make path for cp command
   chdir(p);
   system("mkdir -p cde-package/cde-root/");
   system("chmod -R 755 cde-package/");
   char cpydir[256];
   char cpyfile[256];
   
   //delete at and/exit
   int dae = 0;

   char *input;
   int firstbinary=0; //Check to make first binary the executable in the blob.
   int i;  
   for(i = 1; i < argc; i++) 
   {   
       if(strcmp(argv[i],"-o") == 0)
       { 
         if(i+1 < argc)
           strcpy(outfile, argv[i+1]);
           i++;
         continue;  
       }
       else if(strcmp(argv[i],"-dae") == 0)
       {
         dae = 1;
         continue;  
       }
       else if(strcmp(argv[i],"-d") == 0)
       {

         list_dir(argv[i+1]); 
         i++;
         continue;  
       }
       else if(strcmp(argv[i],"-f") == 0)
       {
         strcpy(cpyfile, "okapi  \"");  
         strcat(cpyfile, argv[i+1]);
         strcat(cpyfile, "\" \"\" ./cde-package/cde-root/");
         system(cpyfile);
         i++;
         continue;  
       }
       else if(strcmp(argv[i],"-static") == 0)
       {
         strcpy(gccstatic, " -static ");
         continue;  
       }
       else 
       { 
           //A hack to check if file is executable as file not permission.
	   char fileisexec[256];
           sprintf(fileisexec,"file -L %s | grep -i -e 'shared object' -e executable", argv[i]);
	   FILE *pipe = popen(fileisexec,"r");
	   char buffer[2048] = "";
	   fgets(buffer,2048,pipe);
	   fclose(pipe); 
	   if(strlen(buffer) > 5) 
	   {
	      if(firstbinary == 0){ input = argv[i]; firstbinary = 1;}

              strcat(command, argv[i]);
              strcat(command, " ");             
	   } 
	   else 
	   {   
	      printf("%s does not seem to be an executable file.\nPlease check your arguments.\n", argv[i]);
	      exit(0);  
	   }
       }        
   }
   strcat(command, " ");

   printf("Making portable executable blob, wait up...\n");
  
   char cwdcmd[256];
   strcpy(cwdcmd, "cd ");
   strcat(cwdcmd, cwd);

   char execfile[256];
   strcpy(execfile, "/tmp/");
   strcat(execfile, p);
   strcat(execfile, "/execute.cde");
   FILE * execfp;
   execfp = fopen (execfile, "w");
   fprintf (execfp, "#!/bin/sh");
   fprintf (execfp, "\n");
   fprintf (execfp, "DN=\"$(dirname \"$(readlink -f \"${0}\")\")\"");
   fprintf (execfp, "\n");
   fprintf (execfp, "$DN/cde-exec %s \"$@\"", input);
   fprintf (execfp, "\n");
   fclose (execfp);
   
   //Write blob assembler file
   char blob[256];
   strcpy(blob,"    .incbin \"");
   strcat(blob, "/tmp/");
   strcat(blob, p);
   strcat(blob, "/blob.tar.gz\"");
   char blobgzS[256];
   strcpy(blobgzS, "/tmp/");
   strcat(blobgzS, p);
   strcat(blobgzS, "/blobgz.S");
   execfp = fopen (blobgzS, "w");
   fprintf (execfp, "    .global blob_tar");
   fprintf (execfp, "\n");
   fprintf (execfp, "    .global size_tar");
   fprintf (execfp, "\n");
   fprintf (execfp, "    .section .rodata");
   fprintf (execfp, "\n");
   fprintf (execfp, "blob_tar:");
   fprintf (execfp, "\n");
   fprintf (execfp, blob);
   fprintf (execfp, "\n");
   fprintf (execfp, "1:");
   fprintf (execfp, "\n");
   fprintf (execfp, "size_tar:");
   fprintf (execfp, "\n");
   fprintf (execfp, "    .int 1b - blob_tar");
   fprintf (execfp, "\n");
   fclose (execfp);

   char chmod[256];
   strcat(chmod, "chmod -R 755 /tmp/");
   strcat(chmod, p);  
   
   system(chmod);

   char exedir[256];
   strcpy(exedir, "/tmp/");
   strcat(exedir, p);    
   chdir(exedir);
   system(command);

   char mvexecfile[256];
   strcpy(mvexecfile, "mv ");
   strcat(mvexecfile, execfile);
   strcat(mvexecfile, " /tmp/");
   strcat(mvexecfile, p);
   strcat(mvexecfile, "/cde-package/execute.cde"); 
   system(mvexecfile);  

   //rm cde.log
   char rmfile[256];
   strcpy(rmfile, "rm  /tmp/");
   strcat(rmfile, p);
   strcat(rmfile, "/cde-package/cde.log");
   system(rmfile);    
   //rm cde.uname
   strcpy(rmfile, "rm  /tmp/");
   strcat(rmfile, p);
   strcat(rmfile, "/cde-package/cde.uname");
   system(rmfile);

   char gnudir[1024];
   sprintf(gnudir,"/tmp/%s/cde-package/cde-root/usr/lib/x86_64-linux-gnu",p );
 
  //Debian based linuxes now have a new place to store libs...
   if (!access(gnudir, F_OK) == 0)  
   {
       chdir("cde-package/cde-root/usr/lib/");
       //system("pwd");     
       system("ln -s ../lib64 ./x86_64-linux-gnu");
       chdir("../../../.."); 
       //system("pwd");
   }
   //And the other way around (we are on Debian based system)
   else    
   {
       chdir("cde-package/cde-root/usr/");
       system("pwd");
       system("cp ./lib64/* ./lib/x86_64-linux-gnu/");
       system("mv lib64 lib64.orig");     
       system("ln -s ./lib/x86_64-linux-gnu ./lib64");
       chdir("../../.."); 
   }

   //make blob.tar.gz
   char targz[256];
   //chdir
   strcpy(targz, "cd /tmp/");
   strcat(targz, p);
   system(targz);

   strcpy(targz, "tar -zcf "); //-zcvf
   strcat(targz, "blob.tar.gz ");
   strcat(targz, "cde-package/");
   system(targz);       

   chdir(startdir);

   char compile[512];
   strcpy(compile, " /tmp/");
   strcat(compile, p);
   strcat(compile, "/gcc.cde ");

   strcat(compile, " -L/tmp/");
   strcat(compile, p);
   strcat(compile, " -I/tmp/");
   strcat(compile, p);
   strcat(compile, " /tmp/");
   strcat(compile, p);

   //Check if we want to compile for delete at exit
   if( dae == 0)
     strcat(compile, "/mkblobexec.o ");
   else 
     strcat(compile, "/mkblobexec_dae.o ");

   strcat(compile, "/tmp/");
   strcat(compile, p);
   strcat(compile, "/blobgz.S ");
   strcat(compile, gccstatic);
   strcat(compile, " -o ");

   strcat(compile, "/tmp/");
   strcat(compile, p);
   strcat(compile, "/");

   strcat(compile, outfile);
   strcat(compile, gccpthread); //must come at the end
   system(compile);
   
   //Move blob to mkblob dir 
   char moveblob[1024];
   strcpy(moveblob, "mv /tmp/");
   strcat(moveblob, p);
   strcat(moveblob, "/");
   strcat(moveblob, outfile);
   strcat(moveblob, " ");
   strcat(moveblob, startdir);
   strcat(moveblob, outfile);
   system(moveblob);

   char rmdir[256];
   strcpy(rmdir, "rm -rf /tmp/");
   strcat(rmdir, p);    
   system(rmdir); 
 
   printf("\nDone!\n");
  
   return 0;
      
}
