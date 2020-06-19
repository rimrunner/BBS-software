

//Compile: no any special flags needed, just gcc
//Usage: When the program is running use another terminal-emulator as a client by typing: telnet <computer's hostname> 3490
//userdata -file needs to be in the same folder

//Note: message areas and download areas are not implemented. Commands which work are j (join conference), l (list conferences), q (quit), s (show userinfo). This project features nonethless comminaction between this server side program and a pure telnet client without any client application as well as the basic functioning of a BBS user database and command system.
//Finnish comments are programmer's personal notes and would be removed if this code was released

//socket(), socket descriptor - int ei aina ole integer arvo. Se on tarpeeksi suuri datavaranto varastoimaan handleseja, joka on intin kokoinen pointteri-data-tyyppi. Ja myös esim se close() menee jotenkin niiden pointtereiden läpi. Socket descriptor osoittaa johonkin datastruktuuriin, jossa on näitä datoja, mitä näissä koodeissa käsitellään. Tämä datastruktuuri on ilmeisesti tuo file descriptor table tai ainakin nämä kaksi ovat yhteydessä toisiinsa, jota käyttis pitää joka prosessille. Tuossa taulukossa on pointteri, joka osoittaa tiedostoon (siis nettiyhteyteen?), jonka prosessi on avannut ja pointteri palautetaan sitä kutsuneelle funktiolle.

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>  //socklen_t
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

//Nämä muuttujiksi
#define PORT "3490" //Portti, johon käyttäjät yhdistetään
#define BACKLOG 10  //Kuinka monta odottavaa yhteyttä jono voi kantaa
#define ULINE_LEN 100
#define RECEIVE_BUFFER_SIZE 64

//int newline2 = 10;
//char newline = newline2;

int password_tries;
char *prompt;
char *bbsname;
char *welcomemsg;
char *quitmsg;
char *invmsg;  //When invalid command is typed
char *menu;


int check_userdata(int varnum, char *varstring, char *username, int use_username);
int load_userdata(int FDf);

/*********************************************
/ The linked list of connected user structs  /
/********************************************/

//Every FD connection has a user struct, it is properly filled when the user gets identity but contains some important data right from the start
//NOTE: For the sake of clarity this struct is recommended to be written to contain variables always in the same order as the user database file (which contains corresponding variables in lines as words separated by two spaces - one line for each user).
struct user {
  char *username;      //0 field numbers used in several functions
  char *password;      //1
  int FD_num;          //2
  char *userlevel;     //3
  char *country;       //4
  char *hometown;      //5
  int totaltime;       //6
  int logtime;         //7 (note: the load_userdata() does not read this last stored value, so it must be logtime (since it is not extracted when logging in))
  int conf;
  int pwtries;
  int (*next_func)(int FD);     //Pointer to next function, if the user is using a command consisting of several functions
  struct user* next; //Pointer to the next user struct in the linked list of logged users and anonymous connections
};

struct user* listhead; //points to the beginning of the list
int update_userbase(struct user *write_this, char add_or_update);

//huom. linked list tutorialin termejä muuteltu: node = struct user, data = FD_num, new_node = new_user, prepend = add_to_userlist, create = do_user_struct, head = listhead

//This list's function creates a user_struct. This is not directly called but the next function calls this.
//First every user gets "new user" datas, but they will be changed after the user is identified
struct user* do_user_struct(int FD_num2) {
  //  struct user* do_user_struct(int FD_num2, struct user* next) {  /LISTHEAD!!
  struct user *new_user = (struct user*)malloc(sizeof(struct user));
  if(new_user == NULL) perror("Error: Cannot create a new user struct\n");

  new_user->FD_num = FD_num2;
  new_user->next = listhead;

  char *anonymous;
  anonymous = malloc(10); //letters of "anonymous" + 1 for null terminator
  strcpy(anonymous, "anonymous");
  anonymous[9] = '\0';
  new_user->username = anonymous;
  char *in_logger;
  in_logger = malloc(10); //characters of "in_logger" + 1 for null terminator
  strcpy(in_logger, "in_logger");
  in_logger[9] = '\0';

  new_user->logtime = time(0);
  new_user->password = NULL;  //It is crucial that all the C strings here are either NULL or malloced since they are later free()d
  new_user->country = NULL;
  new_user->hometown = NULL;
  new_user->userlevel = in_logger;
  new_user->pwtries = 0;
  new_user->conf = 0;
  new_user->next_func = NULL;

  return new_user;
}

//Tätä ilmeisesti kutsutaan ja tämä kutsuu sitten tuota edellistä funktiota
//Listhead siirtyy aina (ei-tyhjän listan tapauksessa) yhden taakse päin, kun uusi struct on luotu
//Tarviiko listheadia parametriksi, kun se on globaali muuttuja?
//HUOM!! Palauttaa listheadin, mutta tarvitaanko sitä?
int add_to_userlist(int FDn) {
  printf("Lisätään userlistiin FD %d\n", FDn);
  //struct user* add_to_userlist(struct user *listhead, int FD_num) {  LISTHEAD!!
  struct user *new_user = do_user_struct(FDn);
  listhead = new_user;
  return 0;
}

//Returns a pointer to a user struct which contains a given FD. IMPORTANT: This function must not be used directly when updating an element which originates from recvbuf (data typed by user - it is converted into a new dynamic string) or is another kind of string created with malloc. Instead use update_user_struct in those cases.
struct user* get_user_struct(int FDstruct) {      //Tässä oli listhead parametrina, mutta ei kai sitä tarvitse
  printf("get_user_struct, haetaan FD:tä %d\n", FDstruct);
  struct user* target = listhead;
  while(target != NULL) {
    //    printf("get_user_struct, USERNAME: %s\n", target->username);
    //    if(target->FD_num == FDstruct) printf("löytyi struct user\n");
    if(target->FD_num == FDstruct) return target;
    target = target->next;
  }
  printf("ei löytynyt struct user\n");
  return NULL;
}

//This function exists because some client input is to be freed immediately and some is used for long periods. So it makes things clearer if this function is always used when storing client input or any other malloc-originating data to structs. 
int update_user_struct(int FDf, char *new_data, int fieldno) {
  char *new_string;
  new_string = malloc(strlen(new_data)+1); //strlen does not include null terminator
  strcpy(new_string, new_data);  //strcpy copies the null terminator as well

  if(fieldno == 0) {
    free(get_user_struct(FDf)->username);
    get_user_struct(FDf)->username = new_string;
  }
  else if(fieldno == 1) {
    free(get_user_struct(FDf)->password);
    get_user_struct(FDf)->password = new_string;
  }
  else if(fieldno == 3) {
    free(get_user_struct(FDf)->userlevel);
    get_user_struct(FDf)->userlevel = new_string;
  }
  else if(fieldno == 4) {
    free(get_user_struct(FDf)->country);
    get_user_struct(FDf)->country = new_string;
  }
  else if(fieldno == 5) {
    free(get_user_struct(FDf)->hometown);
    get_user_struct(FDf)->hometown = new_string;
  }

  /*
  char *username;      //0
  char *password;      //1
  int FD_num;          //2
  char *userlevel;     //3
  char *country;       //4
  char *hometown;      //5
  int totaltime;       //6
  int logtime;         //7
  */
  
}

//DEBUG - funktio joka laskee linked-listin sisällön
int laske_lista() {
  struct user* cursor = listhead;
  int c = 0;
  while(cursor != NULL) {
    c++;
    cursor = cursor->next;
  }
  return c;
}

//Remove the list member on exit (quit or lost connection)
int remove_on_exit(int FDf) {
  if(memcmp(get_user_struct(FDf)->userlevel, "in_logger", 9) != 0) {
    get_user_struct(FDf)->totaltime += time(0) - get_user_struct(FDf)->logtime; //Updating total time
    if(update_userbase(get_user_struct(FDf), 'u') == 1) {
      fprintf(stderr, "update_userbase() failed on exit for user %d", get_user_struct(FDf)->username);
    }
  }
  int retval = 1;
  struct user* us = get_user_struct(FDf);
  free(us->username);
  free(us->password);
  free(us->userlevel);
  free(us->country);
  free(us->hometown);
  //If the struct to be removed is in the beginning of the list
  if(listhead->FD_num == FDf) {
    struct user *first = listhead;
    listhead = listhead->next;
    first->next = NULL;
    if(first == listhead) listhead = NULL; //If the list becomes empty
    free(first);
    retval = 0;
  }
  //If the struct to be removed is in the end of the list
  else if(get_user_struct(FDf)->next == NULL) {
    struct user *last_struct = listhead;
    struct user *second_last = NULL;
    while(last_struct->next != NULL) { //Advancing until last_struct is the last and second_last is the second last
      second_last = last_struct;
      last_struct = last_struct->next;
    }
    if(second_last != NULL) second_last->next = NULL;
    if(last_struct == listhead) listhead = NULL; //If the list becomes empty
    free(last_struct);
    retval = 0;
  }
  //If the struct to be removed is in the middle of the list
  else {
    struct user *survivor = listhead;
    while(survivor != NULL) {
      if(survivor->next->FD_num == FDf) break;
      survivor = survivor->next;
    }
    if(survivor != NULL) { //Miten voisi olla NULL?
      struct user *victim = survivor->next;
      survivor->next = victim->next;  //Setting remaining struct's next pointer to point over the struct to be removed
      victim->next = NULL; //Miksi tämä?
      free(victim);
      retval = 0;
    }
  }
  return retval;
}


/******************************
/         CONFERENCES         /
/*****************************/

struct conference {
  int number;
  char *name;
  int cr_date;
  int permission_level;
  struct filelist *flp;
  struct msglist *mlp;
};

//struct conference *cl_pointer;

struct filedata {
  char *name;
  char *filename;
  char *description;
  int cr_date;
};


/********************
/ User commands     /
/*******************/

//Commands communicating with the user consist of multiple functions and each part of a function leaves a pointer to command's next function to the user struct. When any data is received from a connection/user, the program checks (in the main listening loop) if there is a next_func pointer which means that a command is being executed 
//Commands consisting of multiple functions are in reverse order since the first functions declare function pointers to the latters (that must have been already declared). However, there are cases when separate function declarations are still needed.
//Functions have suffix _f after their names to differentiate their names from their responding command structs.

//Struct for each command
struct command {
  char *comstring;    //What user types to invoke command
  char *altstring;    //Alternative command string
  char *parameter1;
  char *parameter2;
  int (*call)(int FD, char *comstring);      //Pointer to command's function
  int (*call_args)(int FD, char *comstring);      //Pointer to command's function
};

//Sending prompt to the user
void sendpr(int FDpr) {
  send(FDpr, prompt, strlen(prompt), 0);
}

void sendmenu(int FDf) {
  //Tähän voisi tehdä, että se käyttää tuota bbsnamea, mutta sitä varten pitäisi tehdä toinen stringi, jonka pituus tehdään suhteessa BBSnameen, että se menee kohdilleen
  char menu[300]; //40 characters counted for username, userlevel, country, hometown, 20 for totaltime, logtime
  snprintf(menu, sizeof(menu), "\n----------------------------\n|                          |\n| BBS Main menu            |\n|                          |\n| (l)ist conferences       |\n| (j)oin conference        |\n| (s)how your userinfo     |\n| (q)uit                   |\n|                          |\n----------------------------\n\n", bbsname);
  //  snprintf(userinfo, sizeof(userinfo), "Username: %s", (get_user_struct(FDf)->username));
  send(FDf, menu, strlen(menu), 0);
}


int quit_f2(int FDq2, char *com) {
  if(memcmp(com, "yes", 3) == 0 || memcmp(com, "y", 1) == 0) {
    send(FDq2, quitmsg, strlen(quitmsg), 0);
    char *uname = get_user_struct(FDq2)->username;
    printf("Connection %d, user %s has exited through quit\n", FDq2, uname);
    close(FDq2);
    if(remove_on_exit(FDq2) != 0) perror("Remove_on_exit function failed");
    return 99;
  }
  else if(memcmp(com, "no", 2) == 0 || memcmp(com, "n", 1) == 0) {
    get_user_struct(FDq2)->next_func = NULL;
    sendpr(FDq2);
    return 0;
  }
  get_user_struct(FDq2)->next_func = NULL;
  return 1;
}
int quit_f(int FDquit, char *dummy) {
  //debuggerin mukaan FD on 1 (kun sitä ei ole siirretty eli joku alustamaton arvo?)
  char *query = "Are you sure you want to quit? (y/n)\n";
  if(send(FDquit, query, strlen(query), 0) == -1) perror("Quit confirmation fail");
  //Voisiko kaksi seuraavaa riviä yhdistää?
  int (*nf)() = &quit_f2;
  get_user_struct(FDquit)->next_func = nf;
  return 0;
}

//Command which shows user's own info
int show_userinfo_f(int FDf, char *dummy) {
  int temptotal = get_user_struct(FDf)->totaltime + (time(0) - get_user_struct(FDf)->logtime);
  char *testi = get_user_struct(FDf)->username;
  char userinfo[200]; //40 characters counted for username, userlevel, country, hometown, 20 for totaltime, logtime
  snprintf(userinfo, sizeof(userinfo), "\nYOUR USER INFO\nUsername: %s\nUser level: %s\nCountry: %s\nHometown: %s\nLogged in at %d\nTotal time: %d seconds\n\n", get_user_struct(FDf)->username, get_user_struct(FDf)->userlevel, get_user_struct(FDf)->country, get_user_struct(FDf)->hometown, get_user_struct(FDf)->logtime, temptotal);
  //  snprintf(userinfo, sizeof(userinfo), "Username: %s", (get_user_struct(FDf)->username));
  send(FDf, userinfo, strlen(userinfo), 0);
  sendpr(FDf);
  return 0;
}

int join_conf_f2(int FDf, char *confnum, int cl_length, struct conference *cl_pointer) {
  int confn = atoi(confnum);
  for(int i = 0; i < cl_length; i++) {
    if(cl_pointer[i].number == confn) {
      get_user_struct(FDf)->conf = confn;
      char a1[40];
      snprintf(a1, sizeof(a1), "\nYou are now in conference %d\n", confn);
      send(FDf, a1, strlen(a1), 0);
      sendpr(FDf);
      return 0;
    }
  }
  char *a2 = "No conference found for a given number\n";
  send(FDf, a2, strlen(a2), 0);
  sendpr(FDf);
  return 0;
}
int join_conf_f(int FDf, char *dummy) {
  return 96;
}

int list_confs_f2(int FDf, int cl_length, struct conference *cl_pointer) {
  char *a2 = "\nConferences:\n";
  send(FDf, a2, strlen(a2), 0);
  int numbseek = 1; //Counts which potential conference numbers have been already dealt
  int cll_counter = cl_length; //For counting iterations
  //Loop goes through every potential number as long as the number of conferences has been found which corresponds the length of conference array
  for(int i = 0; cll_counter > 0; i++) {  //j is 0-based, cll_counter = 1-based
    if(i >= cl_length) {
      i = 0;
      numbseek++;
    }
    if((cl_pointer[i]).number == numbseek) {
      char a1[50];
      snprintf(a1, sizeof(a1), "%d - %s\n", (cl_pointer[i]).number, (cl_pointer[i]).name);
      send(FDf, a1, strlen(a1), 0);
      cll_counter--;
      numbseek++;
      i = 0;
      continue;
    }
  }
  sendpr(FDf);
  return 0;
}
int list_confs_f(int FDf, char *dummy) {
  return 97; //97 = call list_confs_f2 & send pointer to array and its length 
}

int oneliner_f() {} //Tähän ei tarvita kuin yksi funktio (tyhjä rivi = cancel)

struct command join_conf = {.comstring = "j", .altstring = "join", .call = join_conf_f };
struct command list_confs = {.comstring = "l", .altstring = "list conferences", .call = list_confs_f };
struct command oneliner = {.comstring = "o", .altstring = "oneliner", .call = oneliner_f };
struct command quit = {.comstring = "q", .altstring = "quit", .call = quit_f };
struct command show_userinfo = {.comstring = "s", .altstring = "show info", .call = show_userinfo_f };

//The pointer array for commands, sort this manually
//Note: With some compiler versions it is possible to make simpler solution with the array of command structs. But GCC 5.5.0 and clang 3.8.0 seem not to allow this
struct command *comarray[5] = {&join_conf, &list_confs, &oneliner, &quit, &show_userinfo};


/********************************************************
/  Login and create account                             /
/  (Implemented like commands but cannot be called)     /
/*******************************************************/

//Returns 1 if a string contains two consequential spaces
int check_doubles(char* data_given) {
  for(int i = 0; i <= strlen(data_given); i++) {
    if(data_given[i] == ' ' && data_given[i+1] == ' ') {
      return 1;
    }
  }
  return 0;
}

int create_account_f5(int FDf, char *data_typed) {
  get_user_struct(FDf)->next_func = NULL;
  sendmenu(FDf);
  sendpr(FDf);
}
int create_account_f4(int FDf, char *data_typed) {
  if(memcmp(data_typed, "cancel", 1) == 0) {
    return 98; //recalls login_f()
  }
  else if(check_doubles(data_typed) == 1) {
    char *a1 = "The hometown must not contain two consequential spaces. Type another username or 'cancel' to restart the dialogue.\n";
    send(FDf, a1, strlen(a1), 0);
    return 0;
  }
  else {
    update_user_struct(FDf, "regular user", 3);
    update_user_struct(FDf, data_typed, 5);
    if(update_userbase(get_user_struct(FDf), 'a') == 1) {
      fprintf(stderr, "update_userbase() failed while registrating");
      char *a4 = "The system failed to save your userdata. It will retry when logging out.\n";
      send(FDf, a4, strlen(a4), 0);
    }
    char *a2 = "Registration complete.\n";
    send(FDf, a2, strlen(a2), 0);
    show_userinfo_f(FDf, NULL);
    char *a3 = "Type something to continue\n";
    send(FDf, a3, strlen(a3), 0);
    int (*nf)() = &create_account_f5;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
}
int create_account_f3(int FDf, char *data_typed) {
  if(memcmp(data_typed, "cancel", 1) == 0) {
    return 98; //recalls login_f()
  }
  else if(check_doubles(data_typed) == 1) {
    char *a1 = "The country must not contain two consequential spaces. Type another username or 'cancel' to restart the dialogue.\n";
    send(FDf, a1, strlen(a1), 0);
    return 0;
  }
  else {
    update_user_struct(FDf, data_typed, 4);
    char *a2 = "Type the name of your hometown.\n";
    send(FDf, a2, strlen(a2), 0);
    int (*nf)() = &create_account_f4;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
}
int create_account_f2(int FDf, char *pw_typed) {
  if(memcmp(pw_typed, "cancel", 1) == 0) {
    return 98; //recalls login_f()
  }
  else if(check_doubles(pw_typed) == 1) {
    char *a1 = "The password must not contain two consequential spaces. Type another username or 'cancel' to restart the dialogue.\n";
    send(FDf, a1, strlen(a1), 0);
    return 0;
  }
  else {
    update_user_struct(FDf, pw_typed, 1);
    char *a2 = "Type the country of your residence.\n";
    send(FDf, a2, strlen(a2), 0);
    int (*nf)() = &create_account_f3;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
}
int create_account_f(int FDf, char *name_typed) {
  if(memcmp(name_typed, "cancel", 1) == 0) {
    return 98; //recalls login_f()
  }
  else if(check_userdata(0, name_typed, "", 0) == 1) {
    char *a1 = "The given username already exists. Type another username or 'cancel' to restart the dialogue.\n";
    send(FDf, a1, strlen(a1), 0);
    return 0;
  }
  else if(check_doubles(name_typed) == 1) {
    char *a2 = "The username must not contain two consequential spaces. Type another username or 'cancel' to restart the dialogue.\n";
    send(FDf, a2, strlen(a2), 0);
    return 0;
  }
  else {
    char a3[50];
    snprintf(a3, sizeof(a3), "Your chosen username is %s.\n\n", name_typed);
    send(FDf, a3, strlen(a3), 0);
    update_user_struct(FDf, name_typed, 0);
    char *a4 = "Now enter your password.\nWARNING: The password system is yet experimental (no encryption). Do not use the same password elsewhere.\n";
    send(FDf, a4, strlen(a4), 0);
    int (*nf)() = &create_account_f2;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
}

int login_f(int FDf);

int login_f4(int FDf, char *password) {
  if(check_userdata(1, password, get_user_struct(FDf)->username, 1) != 1) {
    get_user_struct(FDf)->pwtries++;
    if(get_user_struct(FDf)->pwtries < password_tries) {
    char *a1 = "Invalid password. Type your password again.\n";
    send(FDf, a1, strlen(a1), 0);
    }
    else {
      char *a2 = "Password try limit exceeded. Connection closed.\n";
      send(FDf, a2, strlen(a2), 0);
      printf("Connection %d, user %s has exceeded the password try limit. Connection closed.\n", FDf, get_user_struct(FDf)->username);
      close(FDf);
      if(remove_on_exit(FDf) != 0) perror("remove_on_exit() failed");
      return 99;
    }
  }
  else {
    char *a2 = "Login succesful.\n";
    send(FDf, a2, strlen(a2), 0);
    get_user_struct(FDf)->next_func = NULL;
    if(load_userdata(FDf) != 0) fprintf(stderr, "error on function: load_userdata() failed.");
    sendmenu(FDf);
    sendpr(FDf);
  }
  return 0;
}
int login_f3(int FDf, char *name_typed) {
  if(memcmp(name_typed, "cancel", 1) == 0) {
    return 98; //recalls login_f()
  }
  else if(check_userdata(0, name_typed, "", 0) != 1) {
    char *a1 = "The username is not found. Insert username again or type 'cancel' to restart the dialogue.\n";
    send(FDf, a1, strlen(a1), 0);
    return 0;
  }
  else {
    update_user_struct(FDf, name_typed, 0);
    char *a1 = "Enter your password.\n";
    send(FDf, a1, strlen(a1), 0);
    int (*nf)() = &login_f4;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
}
int login_f2(int FDf, char *com) {
  if(memcmp(com, "l", 1) == 0) {
    char *q1 = "What is your username?\n";
    send(FDf, q1, strlen(q1), 0);
    int (*nf)() = &login_f3;
    get_user_struct(FDf)->next_func = nf;
    return 0;
  }
  else if(memcmp(com, "c", 1) == 0) {
    char *q2 = "Type a username you want to choose\n";
    send(FDf, q2, strlen(q2), 0);
    int (*nf2)() = &create_account_f;
    get_user_struct(FDf)->next_func = nf2;
    return 0;
  }
  else if(memcmp(com, "q", 1) == 0) {
    quit_f2(FDf, "y");
    return 99;
  }
  else {
    char *q2 = "Please, enter l/c/q\n";
    send(FDf, q2, strlen(q2), 0);
    return 0;
  }
}
int login_f(int FDf) {
  char *q1 = "Do you want to log in as an old user (l), create a new account (c) or quit (q)?\n";
  send(FDf, q1, strlen(q1), 0);
  int (*nf)() = &login_f2;
  get_user_struct(FDf)->next_func = nf;
  return 0;
}

/**************************************
/       User command processing       /
/*************************************/

char* is_there_arg(char *u_input) {
  char *tempstring;
  if(memchr(u_input, ' ', strlen(u_input)) != NULL) { //If command string has space(s)...
    int spacel = strcspn(u_input, " "); //The location of space in the string
    tempstring = malloc(strlen(u_input)-spacel+1);
    int j = 0;
    for(int i = spacel+1; i <= strlen(u_input); i++) {
      tempstring[j] = u_input[i];
      j++;
    }
    tempstring[strlen(u_input)-spacel] = '\0';
  }
  else { //If spaces are not found, copy without modifications
    tempstring = NULL;
  } 
  return tempstring;
}

//A function which takes a first word of a string and converts it to lowercase
char* first_word (char *u_input) {
  char *tempstring;
  if(memchr(u_input, ' ', strlen(u_input)) != NULL) { //If command string has space(s)...
    int spacel = strcspn(u_input, " "); //The location of space in the string
    tempstring = malloc(spacel+1);
    strncpy(tempstring, u_input, spacel);  //No null terminator to copy here
    tempstring[spacel] = '\0';
  }
  else {    //If spaces are not found, copy without modifications
    tempstring = malloc(strlen(u_input)+1);
    strcpy(tempstring, u_input); //This should be null terminated but additional byte needs to be malloced for it
  } 
  //Tempstring is converted to lowercase as tempstring2
  char *tempstring2;
  tempstring2 = malloc(strlen(tempstring)+1);  //Calloc is crucial when building a string from char by char
  //  char tempchar;
  //  char stringchar[1];
  for(unsigned int i = 0; i != strlen(tempstring); i++) tempstring2[i] = tolower(tempstring[i]);
/*
  for(unsigned int i = 0; i != strlen(tempstring); i++) {
    tempchar = tolower(tempstring[i]);
    stringchar[0] = tempchar;
    strncat(tempstring2, stringchar, 1);  
  }
*/
  tempstring2[strlen(tempstring)] = '\0';
  free(tempstring);
  return tempstring2;
}

//A function to check whether a string (presumedly first processed in first_word()) is a user command defined listed as pointers in comarray
//Returns the index number of a command's pointer in comarray that can be used for calling it. -1 = not found
int is_command(char *maybecom) {
  int retval = -1;
  for(int j = 0; j != sizeof(comarray)/sizeof(*comarray); j++) {
    //    if(memcmp(comarray[j]->altstring, maybecom, sizeof(comarray[j]->altstring)) == 0) {
    if(memcmp(comarray[j]->altstring, maybecom, strlen(comarray[j]->altstring)) == 0 || memcmp(comarray[j]->comstring, maybecom, strlen(comarray[j]->comstring)) == 0) {
      retval = j;
      break;
    }
  }
  free(maybecom);
  return retval;
}

/******************************
/       I/O OPERATIONS        /
/*****************************/


//Used when extracting data from a line taken from the userdata file. Reads characters that are between n and previous_gap (used in a loop which calls this function)
char* udata_extract(char *r_array, int previous_gap, int n) {
  char *datastring;
  datastring = malloc(n-previous_gap+1); //+1 = null terminator
   int i = 0;
   for(int p = previous_gap; p != n; p++) {  //Here args are used as 0-based
     datastring[i] = r_array[p];
     printf("udata: %c", datastring[i]);
     i++;
   }
   datastring[n-previous_gap] = '\0';  //here +1 is not needed because it is 0-based
   return datastring;
} 

//Counting the rows of the userbase file
//Note: the last line is not counted if it does not end with \n
int count_udatalines(FILE *fptr) {
  int datalines = 0;
  int datareader;
  while(!feof(fptr)) {  //feof kertoo, ollaanko EOF:issa
    datareader = fgetc(fptr);  //fgetc() lukee seuraavan charin streamista ja palauttaa sen
    if(datareader == '\n') datalines++;
  }
  return datalines;
}

//Stores userdata file's lines to a char array
char (*read_userdata(FILE *fptr, int linenum))[ULINE_LEN] {   //The beginning of a function with the array of strings as a return type
  char (*r_array)[ULINE_LEN] = malloc(linenum * sizeof(*r_array));  //Allocating memory for the array of strings to be returned.
  int counter = 0;
  while(fgets(r_array[counter], ULINE_LEN, fptr)) {
    counter++;
  }
  return r_array;
}

//Takes pointer to userdata string array & username & number of lines in the file and returns a line where the given username appears
int get_userline(int datalines, char *username, char (*r_array)[ULINE_LEN]) {
  int line_found = -1;
  //Finding userdata line-to-be-updated from the userdata file
  for(int i = 0; i <= datalines; i++) {
    if(memcmp(r_array[i], username, strlen(username)) == 0) {
      line_found = i;
      break;
    }
  }
  return line_found;
}

//Takes FD and username from FD's a corresponding user struct and fills the user struct with data attributed to the user in the userdata file 
int load_userdata(int FDf) {
  FILE *userfile_ptr = fopen("userdata", "r+");
  if(userfile_ptr == NULL) perror("Error: Cannot open user struct file\n");
  int lines = count_udatalines(userfile_ptr);
  rewind(userfile_ptr);
  char (*read_array)[ULINE_LEN] = read_userdata(userfile_ptr, lines);
  fclose(userfile_ptr);
  char *uname = get_user_struct(FDf)->username;
  int uline = get_userline(lines, uname, read_array);
  if(uline == -1) {
    perror("Userdata loading error: no matching user found");
    fclose(userfile_ptr);
    free(read_array);
    return 1;
  }
  
  //Going through variables from user's datastring and adding them to their active user_struct
  int gaps = 0; //gaps marked by two consequential spaces which are delimiters in the user datafile
  int previous_gap = 0;
  for(int j = 0; j <= strlen(read_array[uline]); j++) {
    printf("READ_ARRAYN KOKO: %d\n", strlen(read_array[uline]));
    if(read_array[uline][j] == '\\' && read_array[uline][j+1] == 'n') break;
    if(read_array[uline][j] == ' ' && read_array[uline][j+1] == ' ') {
      if(gaps == 0 || gaps == 2) {  //The struct already has the username and we do not store the previous FD number or logtime, so entries 0, 2 are skipped and the loop does not read the last value, logtime (because there are no spaces after it)
	gaps++;
	previous_gap = j+2;
	continue;
      }
      else { //Other variables
	char *tstring = udata_extract(read_array[uline], previous_gap, j);
	if(gaps == 1) update_user_struct(FDf, tstring, 1); //password
	else if(gaps == 3) update_user_struct(FDf, tstring, 3); //userlevel
	else if(gaps == 4) update_user_struct(FDf, tstring, 4); //country
	else if(gaps == 5) update_user_struct(FDf, tstring, 5); //hometown
	else if(gaps == 6) get_user_struct(FDf)->totaltime = atoi(tstring); //totaltime
	gaps++;
	previous_gap = j+2;
	free(tstring);
      }
    }
  }
  free(read_array);
  return 0;
}

//Rewrites the userbase by updating a given struct or adding it as a new user
int update_userbase(struct user *write_this, char add_or_update) {
  FILE *userfile_ptr = fopen("userdata", "r+");
  if(userfile_ptr == NULL) fprintf(stderr, "Error: Cannot open user struct file\n");

  //Counting the rows of the userbase file
  int lines = count_udatalines(userfile_ptr);
  rewind(userfile_ptr);
  
  if(add_or_update == 'a') lines++; //element for a new entry

  char (*read_array)[ULINE_LEN] = read_userdata(userfile_ptr, lines);
  rewind(userfile_ptr);

  char update_str[ULINE_LEN];
  snprintf(update_str, sizeof(update_str), "%s  %s  %d  %s  %s  %s  %d  %d\n", write_this->username, write_this->password, write_this->FD_num, write_this->userlevel, write_this->country, write_this->hometown, write_this->totaltime, write_this->logtime);  //Tämä näyttää jonkun end-characterin tms, mutta se kai poistuu tuossa strlenissä

  if(add_or_update == 'u') {
    char *user = write_this->username;
    //Finding userdata line-to-be-updated from the userdata file
    int update_index = get_userline(lines, user, read_array);
    if(update_index == -1) {
      perror("Userdata loading error: no matching user found");
      fclose(userfile_ptr);
      free(read_array);
      return 1;
    }
    else {
      printf("UPDATE: %s", update_str);
      printf("\n");
      strncpy(read_array[update_index], update_str, strlen(update_str));  //strlen copies the size without null character (here: newline character)
    }
  }

  else if(add_or_update == 'a') strncpy(read_array[lines-1], update_str, strlen(update_str)); //adding new userdata
  
  /*DEBUG: write with printf instead
  for(int j = 0; j != lines; j++) {
    for(int i = 0; i <= 70; i++) {
      if(read_array[j][i] == '\n') break;
      printf("%c", read_array[j][i]);
    }
    printf("\n");
  }
  */

  //Writing userdata back to the file
  for(int j = 0; j != lines; j++) {
    for(int i = 0; i <= ULINE_LEN; i++) {
      if(read_array[j][i] == '\n') break;
      fprintf(userfile_ptr, "%c", read_array[j][i]);
    }
    fprintf(userfile_ptr, "\n");
  }
  
  fclose(userfile_ptr);
  return 0;
}

//Function checks if a given user variable exists in the database file. Fourth parameter can be 0 or 1. If it is 0, function only checks whether searched variable's value exists somewhere in the datafile, if it is 1, the function checks if the variable exists for a given user.
int check_userdata(int varnum, char *varstring, char *username, int use_username) {
  FILE *userfile_ptr = fopen("userdata", "r+");
  if(userfile_ptr == NULL) perror("Error: Cannot open user struct file\n");

  int lines = count_udatalines(userfile_ptr);
  rewind(userfile_ptr);

  //Tämä pitäisi varmaan korjata jotenkin. Tässä ei tule mitään erroria, jos rivi onkin pidempi kuin tuo annettu luku
  char (*read_array)[ULINE_LEN] = read_userdata(userfile_ptr, lines);
  fclose(userfile_ptr);

  int gaps = 0;
  int previous_gap = 0;
  for(int i = 0; i < lines; i++) {
    for(int j = 0; j <= sizeof(read_array[i]); j++) {
      if(read_array[i][j] == ' ' && read_array[i][j+1] == ' ') {
	if(gaps == 0 && use_username == 1) {  //If searching for username to match a value
	  char *tempuser = udata_extract(read_array[i], 0, j); //previous gap = 0 since username starts the line
	  if(memcmp(tempuser, username, strlen(username)) != 0) {
	    //If this is not the username that is being searched, jump to next iteration
	    free(tempuser);
	    gaps++;
	    previous_gap = j+2;
	    continue;
	  }
	  else free(tempuser);
	}
	if(gaps == varnum) {
	  char *tstring = udata_extract(read_array[i], previous_gap, j);
	  printf("tstring: %s", tstring);
	  printf("varstring: %s", varstring);
	  gaps++;
	  previous_gap = j+2;
	  if(memcmp(varstring, tstring, strlen(tstring)) == 0) {
	    free(tstring);
	    return 1;  //The same code works here whether the username is searched or just the variable. Wrong username has already been ruled out.
	  }
	  else free(tstring);
	  gaps = 0;
	  previous_gap = 0;
	  continue;
	}
	else {
	  gaps++;
	  previous_gap = j+2;
	}
      }
    }
    previous_gap = 0;
    gaps = 0;
  }
  return 0;
}


//SOCKET DATA

//Tämä funktio ottaa sockaddrin ja palauttaa IPv4 tai IPv6 sen mukaan, kumpi siellä nyt on
void *get_in_addr(struct sockaddr *sa) {  //Ilmeisesti määrittely tarkoittaa, että tämä on void pointterin (yleiskäyttöisen pointterin) palauttava funktio
  if(sa->sa_family == AF_INET)  return &(((struct sockaddr_in*)sa)->sin_addr);
  return &(((struct sockaddr_in6*)sa)->sin6_addr);
  //Sulkeiden ja nuolen presendenssi on sama, mutta ne luetaan vasemmalta oikealle, joten suluissa oleva struct-osa on kai niinkun, että ensimmäisenä laitetaan pointteriksi sa-muuttuja (sehän on sama, kummassa osapuolessa se asteriski on kiinni, sulut vain merkkaa, että kyse on asteriskista), sitten siihen haetaan sin6_addr ja palautetaan &:n kanssa. Palauttaa osoitteen struct sin[6]_addriin. Joku sanoi, että &x on pointteri (jos x = esim int x) eli tuokin palauttaa siis pointterin (&sin_addr ilmeisesti).
  //parametrinä on pointteri sockaddriin, siitä katsotaan, mikä sen memberin (struct sockaddr_in[6] arvo on. Kyse on ilmeisesti jostain tämmöisestä: http://www.iso-9899.info/wiki/Common_Initial_Sequence
}


int main() {

  //väliaikaiset
  struct conference conflist[3];

  struct conference musique;
  musique.number = 2;
  musique.name = "Musique";
  musique.cr_date = time(0);
  musique.permission_level = 1;
  musique.flp = NULL;
  musique.mlp = NULL;

  struct conference horses;
  horses.number = 3;
  horses.name = "Horses";
  horses.cr_date = time(0);
  horses.permission_level = 1;
  horses.flp = NULL;
  horses.mlp = NULL;

  struct conference astronomy;
  astronomy.number = 5;
  astronomy.name = "Astronomy";
  astronomy.cr_date = time(0);
  astronomy.permission_level = 1;
  astronomy.flp = NULL;
  astronomy.mlp = NULL;


  conflist[0] = musique;
  conflist[1] = horses;
  conflist[2] = astronomy;
  
  password_tries = 3;
  prompt = "enter command: ";
  welcomemsg = "Welcome to FRONTLINE BBS";
  bbsname = "FRONTLINE";
  quitmsg = "Thank you for visiting FRONTLINE\n";
  invmsg = "Invalid command";
  menu = "-----------------\n(L)ist conferences\n(J)oin a conference\n(S)how your useinfo\n(Q)uit\n-----------------\n";

  fd_set master; //Add master to the fd set
  fd_set read_fds; //Temporary file descriptor list used with select()
  int fdmax; //Max file descriptor number
  int listener, new_fd; //Socket descriptorit, joista listener:llä kuunnellaan, new_fd on uutta yhteyttä varten, listener is for connecting //LIIKAA!!
  
  int recvbytes;
  char recvbuf[RECEIVE_BUFFER_SIZE];
  memset(recvbuf, 0, sizeof recvbuf);

  struct addrinfo hints, *servinfo, *p;
  struct sockaddr_storage their_addr; //Yhteydenottajan osoitetiedoille. Tämä struct-tyyppi on tarpeeksi suuri pitämään IPv4 ja IPv6 -structit, jos ei tiedä, kumpaa tarvitaan. Pelkkä sockaddr ei ole riittävän iso
  socklen_t sin_size;  //Tyyppi on vähintään 32-bittinen unsigned int
  int yes=1; //setsockoptille boolelainen arvo inttinä
  char remoteIP[INET6_ADDRSTRLEN];
  int rv; //getaddrinfon tarkistusmuuttuja

  FD_ZERO(&master); //Clearing... FD_ZERO clears all entries from the set
  FD_ZERO(&read_fds);

  memset(&hints, 0, sizeof hints);  //Varmennustyhjennys
  hints.ai_family = AF_UNSPEC;  //IP-versioneutraalius
  hints.ai_socktype = SOCK_STREAM;  //Perus stream socket
  hints.ai_flags = AI_PASSIVE; //Käytä koneen IP:tä (?)

  if((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {  //getaddrinfo on tämä addrinfo-structit täyttävä funktio
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
      return 1;
  }

  /*
  tarpeettomaksi osoittautunut apupointterin käyttö:
  struct command *temp_p;
  temp_p = comarray[0];
  char *testichar = (temp_p)->altstring;
  */
  //  char *testichar = (comarray[1])->altstring;
  //  printf("the first instruction gives %s\n", testichar);

  
  //Luupataan kaikkien getaddrinfon laittamien tulosten läpi ja bindataan ensimmäiseen, mihin voi
  for(p = servinfo; p != NULL; p = p->ai_next) { //p on määritelty aiemmin struct addrinfo -tyyppiseksi pointteriksi ja siihen assignoidaan toinen vastaava, johon on laitettu osoite
    if((listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) { //Tässä assignoidaan socket descriptor -int-muuttuja. Socketista on tämän tekstin alussa lisää. Virheen tapaukessa socket-olio ilmeisesti on aito int, eikä pointer-handle-storage tms.
      perror("server: socket"); //Perror kirjoittaa kuvailevan virheviestin stderr:iin
      continue;  //continue --> loopin seuraavalle kierrokselle breakin sijaan
    }
    if(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {perror("setsockopt"); exit(1);}
    //setsockopt() muokkaa socket descriptorin asetuksia. SOL_SOCKET = muokataan asetuksia API-tasolla, SO_REUSEADDR = Antaa socketin bind()ata porttiin, jos ei ole aktiivista kuuntelevaa socketia siellä jo. Näin voi välttää "address already in use" errorit, jos kaatunut serveri käynnistetään uudelleen. Kolmas parametri on pointteri inttiin, joka on false (0) tai true (>0) ja laittaa siis päälle tai pois edellisen parametrin asetuksen (tai se voi olla joku muukin arvo kaiketi kuin boolelainen) ja viimeinen parametri on edellisen parametrin pituus (usein sizeof(int))

    //Kuuntelusockettiin bindataan lokaalit osoitetiedot ilmeisesti tässä:
    //bindillä sidotaan socket (esim.) oman koneen osoitteeseen
    if(bind(listener, p->ai_addr, p->ai_addrlen) == -1) {
      close(listener); //suljetaan jos virhe
      perror("server: bind");
      continue;
    }
    break;  //Looppi jatkaa, jos bindaus ei onnistu, muuten break heti ekalla kerralla, kun bindattava osoite löytyy
  }

  freeaddrinfo(servinfo); //ei tarvita enää

  //Vielä tarkistetaan, onnistuiko bindaus
  if(p == NULL) {  
    fprintf(stderr, "server: bind failure\n");
    exit(1);
  }

  //Listen päälle. Listen kertoo socketille, että ollaan valmiita hyväksymään yhteyksiä ja antaa maksimiluvun jonotettaville yhteyksille
  if(listen(listener, BACKLOG) == -1) {
    perror("listen");
    exit(1);
  }

  FD_SET(listener, &master); //Adding listener socket to the master set
  fdmax = listener; //At the moment there is no larger socket FD number than listener

  printf("Waiting for connections...\n");

  sin_size = sizeof their_addr;

  for(;;) {  //main loop

    read_fds = master; //Storing master set
    if(select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1) {
      fprintf(stderr, "select() error");
      exit(4);
    }
    //Checking for new connections and readable data
    for(int con_num = 0; con_num <= fdmax; con_num++) {
      if(FD_ISSET(con_num, &read_fds)) { //Returns true if fd (con_num) is set
	if(con_num == listener) { //This means there is a new connection  ...Siis jos listenerin kohdalle on laitettu FD, silloin se on siellä ilmeisesti tilapäisesti ja tarkoittaa uutta yhteyttä
	  sin_size = sizeof their_addr;
	  new_fd = accept(listener, (struct sockaddr*)&their_addr, &sin_size);  //accept() palauttaa yhteyskohtaisen uuden socket file descriptorin. Toinen parametri on struckt sockaddr* addr, ja & -tulee siis, koska se on pointteri-parametri, mutta siinä jotenkin laitetaan argumentissa, että se on tyypiltään struct sockaddr*, kun se syötettävä olio on varsinaisesti struct sockaddr_storage -tyyppinen... olikohan tuo se joku unified initialization struct (???), että noissa structeissa on jotain yhteistä, niin niitä voi käyttää noin. Tuo sockaddr_storagehan on kai vain joku toinen versio sockaddrista, joka on riittävän iso IPv6 -osoitteille
	  if(new_fd == -1) {
	    perror("accept error");
	  }
	  else {
	    FD_SET(new_fd, &master); //A new connection is added to the master set
	    if(new_fd > fdmax) fdmax = new_fd; //Updating fdmax
	    printf("selectserver: New connection from %s on ""socket %d\n", inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr*)&their_addr), remoteIP, INET6_ADDRSTRLEN), new_fd);  //network-tyyppinen binäärikoodaus muutetaan presentaatioksi (koneen ymmärtämäksi). ss_family on joko IPv4 tai IPv6,
	    send(new_fd, "\n", strlen("\n"), 0);
	    if(send(new_fd, welcomemsg, strlen(welcomemsg), 0) == -1) perror("Welcome msg fail");  //Viestin lähetys, kolmas parametri on viestin pituus, neljäs on flag
	    send(new_fd, "\n", strlen("\n"), 0);
	    send(new_fd, "\n", strlen("\n"), 0);
	    sendpr(new_fd);
	    add_to_userlist(new_fd); //At this point con_num is listener socket's con_num that is always 3 (may be system dependent?) so it should not be used as an argument for add_to_userlist(). Instead new_fd gives a socket number that is used also later as a con_num when data is received from the same socket.
	    login_f(new_fd);
	    printf("con_num = %d\n", con_num); //DEBUG
	    printf("fdmax1 = %d\n", fdmax); //DEBUG
	    int lasku1 = laske_lista();
	    printf("listan pituus: %d\n", lasku1);
	  }
	}
	else { //Receiving data instead of a new connection when con_num did not match the listener
	  if((recvbytes = recv(con_num, recvbuf, sizeof recvbuf, 0)) <= 0) {
	    if(recvbytes == 0) {
	      printf("Connection %d exited\n", con_num);
	    }
	    else perror("recv() returned error");
	    close(con_num); //Error or connection lost -> closing that socket and removing from master set
	    FD_CLR(con_num, &master);
	    if(remove_on_exit(con_num) != 0) perror("remove_on_exit() failed");
	  }
	  else { //When data is received...
	    printf("from %d: %s||", con_num, recvbuf);
	    char *bufentry;
	    for(int i = RECEIVE_BUFFER_SIZE-1; i > 0; i--) {  //Reading recvbuf backwards and taking the stuff with some content (instead of 0)
	      if(recvbuf[i] != 0) {
		//printf("bufferin lopussa on %d välilyöntiä\n", RECEIVE_BUFFER_SI>E-1-i);
		bufentry = malloc(i); //at this point i contains an additional value that is a place for the null terminator
		memcpy(bufentry, recvbuf, i-1);
		bufentry[i-1] = '\0'; //here i is 0-based unlike above
		break;
	      }
	    }
	    printf("fdmax2 = %d\n", fdmax); //DEBUG
	    printf("con_num, part 2 = %d\n", con_num); //DEBUG
	    printf("USERNAME in mainloop: %s\n", get_user_struct(con_num)->username);
	    if(bufentry == "") sendpr(con_num); //If the user has just pressed return without typing anything etc. while not in a dialogue
	    else { //If the user has actually typed something 
	      //int lasku2 = laske_lista();
	      //printf("listan pituus: %d\n", lasku2);
	      int (*temp_funcp)() = get_user_struct(con_num)->next_func; 
	      if(temp_funcp != NULL) { //If the user is in the middle of a command consisting of multiple parts
		int comret = temp_funcp(con_num, bufentry);
		if(comret > 0) {
		  if(comret == 99) FD_CLR(con_num, &master); //Quit command returns 99, FD_CLR is called here because "master" is local
		  else if(comret == 98) login_f(con_num);  //Cancel which takes back to the beginning of the login
		  else if(comret == 1) { //Invalid command
		    send(con_num, invmsg, strlen(invmsg), 0);
		    send(con_num, "\n", strlen("\n"), 0);
		    sendpr(con_num);
		  }
		}
	      }
	      else { //A new command instead
		int command_num = is_command(first_word(bufentry));
	        if(command_num >= 0) {  //If is_command returns >= 0, the command is found and called
		  char *comarg = is_there_arg(bufentry); //Comarg becomes NULL, if there are no argument in bufentry
		  int comres = comarray[command_num]->call(con_num, comarg);  //Tähän saa varmaan argumentit sitten, jos ne laittaa tuohon call()iin
		  if(comres == 97) list_confs_f2(con_num, (sizeof(conflist)/sizeof(*conflist)), conflist);
		  if(comres == 96) join_conf_f2(con_num, comarg, (sizeof(conflist)/sizeof(*conflist)), conflist);
		  free(comarg);
		}
		else { //If command not found
		  send(con_num, invmsg, strlen(invmsg), 0);
		  send(con_num, "\n", strlen("\n"), 0);
		  sendpr(con_num);
		}
	      }
	    }
	    free(bufentry);
	    memset(recvbuf, 0, sizeof recvbuf);  //Reseting receive buffer after each entry, otherwise troubles follow immediately
	  }
	}
      }
    }
  }
  return 0;
}
