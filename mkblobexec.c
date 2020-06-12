/*
mkblobexec.c
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
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>

//Do not edit, this is changed at buildtime
int deleteAtExit=0;
 
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



//Lefttrim:
//http://www.martinbroadhurst.com/trim-a-string-in-c.html
char *ltrim(char *str, const char *seps)
{
    size_t totrim;
    if (seps == NULL) {
        seps = "\t\n\v\f\r ";
    }
    totrim = strspn(str, seps);
    if (totrim > 0) {
        size_t len = strlen(str);
        if (totrim == len) {
            str[0] = '\0';
        }
        else {
            memmove(str, str + totrim, len + 1 - totrim);
        }
    }
    return str;
}

//Righttrim
char *rtrim(char *str, const char *seps)
{
    int i;
    if (seps == NULL) {
        seps = "\t\n\v\f\r ";
    }
    i = strlen(str) - 1;
    while (i >= 0 && strchr(seps, str[i]) != NULL) {
        str[i] = '\0';
        i--;
    }
    return str;
}

//General Trim (ltrim and rtrim)
//Call with trim(str, NULL);
char *trim(char *str, const char *seps)
{
    return ltrim(rtrim(str, seps), seps);
}

//Global variables for SIGUSR1 communication.
int mainexepid = -1;
int usr1done = 0;
char rmhome[256];
char exebin[256];

void sig_handler(int signo)
{
  if (signo == SIGUSR1 && usr1done == 0)
  {

   while(1)
   {
       if(0 == system(exebin)) {
          printf("A process having name %s is running. Deleting loaded files.\n", exebin);
          system(rmhome);
          usr1done = 1;   
	  break;
       }
       else if(1 == system(exebin)) {
	  printf("A process having name PROCESS is NOT running.\n");
       }
       usleep(250); 
     }
   } 
}

/*thread function definition*/
void* threadFunction(void* args)
{
  while(1)
   {
       if(0 == system(exebin)) {
          
          if(deleteAtExit == 0) 
            system(rmhome);          
	  break;
       }
       else if(1 == system(exebin)) {
	 // printf("A process having name PROCESS is NOT running.\n");
       }
       usleep(2500); 
   }
}

int main(int argc, char *argv[])
{

   /*creating thread id*/
   pthread_t id;
   int ret;
 
   char cwd[PATH_MAX];
   getcwd(cwd, sizeof(cwd));//current dir
   char startdir[PATH_MAX];
   getcwd(startdir, sizeof(startdir));
   strcat(startdir,"/");

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
  
   chdir("/tmp/");
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
   strcat(command,"; execute.cde ");
 
   int i;
   for(i = 1; i < argc; ++i) 
   {  
       strcat(command, argv[i]);
       strcat(command, " "); 
   }
  
   char cwdcmd[256];
   strcpy(cwdcmd, "cd ");
   strcat(cwdcmd, cwd);

   char chmod[256];
   strcpy(chmod, "chmod -R 755 /tmp/");
   strcat(chmod, p);    
   system(chmod);

   /*creating thread*/
   sprintf(rmhome, "rm -rf /tmp/%s/", p);
   sprintf(exebin, "pidof -x /tmp/%s/cde-root/lib64/ld-linux-x86-64.so.2  > /dev/null", p);
   ret=pthread_create(&id,NULL,&threadFunction,NULL);
   if(ret==0){
      // printf("Thread created successfully.\n");
   }
   else{
       // printf("Thread not created.\n");
       return 0; //return from main
   }
   
   //Change to start dir
   chdir(cwd);
   system(command);

   char rmdir[256];
   strcpy(rmdir, "rm -rf /tmp/");
   strcat(rmdir, p);
   system(rmdir); 

   return 0;
      
}
