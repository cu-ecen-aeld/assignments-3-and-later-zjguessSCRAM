#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

int main(int argc, char *argv[]){
  openlog("writer", 0,  LOG_USER);
  
  // Ensure that exactly two arguments were provided
  if (argc != 3){
    syslog(LOG_ERR, "Usage: %s <file> <string>", argv[0]);
    closelog();
    return 1;
  }
  
  const char *filePath =argv[1];
  const char *textString = argv[2];

  FILE *fp = fopen(filePath, "w");
  
  // 
  if (fp == NULL){
    syslog(LOG_ERR, "Error opening file: %s", filePath);
    fclose(fp);
    closelog();
    return 1;
  }
  
  if (fputs(textString, fp) == EOF){
    syslog(LOG_ERR, "Error writing to file: %s", filePath);
    fclose(fp);
    closelog();
    return 1;
  }
  
  syslog(LOG_DEBUG, "Writing %s to %s", textString, filePath);
  
  fclose(fp);
  closelog();
  return 0;  
}
