#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

//define
#define MAX_USERS 1000 //max number of users possible
#define MAX_ROOMS 1000 //max number of rooms possible
#define TIMEOUT 300 //5 minute timeout for each client 

typedef struct room room;

//user structure
struct user {
    char *name;
    struct room *userRoom;
    int clntSock;
}User;

//room structure
struct room {
    char *name;
    char *password;
    int numUsers;
    struct user *users[MAX_USERS];
}Room;

//GLOBAL (server) variables
int counter = 0;
struct user *allUsers[MAX_USERS]; //will hold all users on the server
struct room *allRooms[MAX_ROOMS]; //will hold all rooms on the server
pthread_mutex_t joinLeaveNickLock; //mutex for joining and leaving rooms and changing your name
int numOfUsers;
int numOfRooms;

int nameAlreadyExists(char *name) {
    for(int i = 0; i < MAX_USERS; i++) 
        if(allUsers[i] == NULL) continue;
        else if(strcmp(allUsers[i]->name, name) == 0) return 1;
    return 0;
}

int getNextOpenUserIndex(struct user *users[MAX_USERS]) {
    for(int i = 0; i < MAX_USERS; i++) 
        if(users[i] == NULL) return i;
    return -1;
}

int getNextOpenRoomIndex() {
    for(int i = 0; i < MAX_ROOMS; i++)
        if(allRooms[i] == NULL) return i;
    return -1;
}

void sendMessage(int clntSock, char *response, int responseLength) {
    int bytesSent = 0;
    while(bytesSent < responseLength) {
        int temp = bytesSent;
        bytesSent += send(clntSock, response + bytesSent, responseLength - bytesSent, 0);
        if(temp > bytesSent) {
            fprintf(stderr, "ERROR: server sending response to client (join) - failed");
            exit(1);
        }
    }
}

void deleteRoom(struct room *currRoom) {
    int currNumOfRooms = numOfRooms;
    for(int i = 0; i < MAX_ROOMS; i++) {
        if(currNumOfRooms == 0) {
            fprintf(stderr, "ERROR: attemped to delete a non existent room");
            break;
        }
        if(allRooms[i] != NULL) {
            currNumOfRooms--;
            if(strcmp(allRooms[i]->name, currRoom->name) == 0) {
                //check if there are no users in here
                if(currRoom->numUsers != 0) {
                    fprintf(stderr, "ERROR: trying to delete a room that is not empty\n");
                    return;
                }
                free(currRoom->name);       //free room name ... THESE FREE CALLS MAY CAUSE PROBLEMS (SEG FAULT)
                free(currRoom->password);   //free room password
                allRooms[i] = NULL;         //erased room from server's list of rooms
                numOfRooms--;               //decrement number of rooms on the server

                return;
            }
        }
    }
    fprintf(stderr, "ERROR: attempted to delete a room that didnt exist on the server\n");
}

int handleConnect(int clntSock, char *message) {
    //create buffer to store information we are going to send back
    char *response;
    message[5] = '\0'; //null terminate the message

    if(!(strcmp(message, "Hello\0") == 0)) {
        fprintf(stderr, "ERROR: recieved unexpected payload from message of opCode 0xff.\n");
        return -1; //we just return if the payload is unexpected
    }

    //------- altering users global array, lock the mutex! --------
    pthread_mutex_lock(&joinLeaveNickLock);

    int index = getNextOpenUserIndex(allUsers);
    if(index == -1) return -1; //if there is NOT an open spot for a new user then return (prob is not tested)

    allUsers[index] = malloc(sizeof(User));
    allUsers[index]->name = malloc(256);                //set user name
    sprintf(allUsers[index]->name, "rand%d", index);
    allUsers[index]->userRoom = NULL;                   //set the user room to NULL denoting lobby
    allUsers[index]->clntSock = clntSock;

    numOfUsers++;                               //increment the numOfUsers
    
    pthread_mutex_unlock(&joinLeaveNickLock);
    //------------------ done! unlock mutex! ----------------------

    //constructing response
    int responseLength = 8 + (int)strlen(allUsers[index]->name) + 1;        //total length of response
    response = malloc(responseLength);                              //allocate memory for response
    *(uint16_t *)&response[0] = htons(0x417);                       //first 2 bytes are 0x417
    *(uint32_t *)&response[2] = htonl(strlen(allUsers[index]->name) + 1);   //next 4 bytes hold length of payload of response
    *(uint8_t *)&response[6] = 0xfe;                                //next 1 byte holds the opcode for all server responses, 0xfe
    *(uint8_t *)&response[7] = 0;                                   //next 1 byte holds the index of the next arg
    sprintf(&response[8], "%s", allUsers[index]->name);                     //next number of bytes holds the users assigned name

    responseLength--; //this is because we dont want to send the extra byte holding the null terminator 
   
    //send message back to client
    int bytesSent = 0;
    while(bytesSent < responseLength) {
        int temp = bytesSent;
        bytesSent += send(clntSock, response + bytesSent, responseLength - bytesSent, 0);
        if(temp > bytesSent) {
            fprintf(stderr, "ERROR: server sending response to client - failed");
            exit(1);
        }
    }
    free(response);
    return index;
}

void handleJoin(int clntSock, char *message, struct user *currUser) {
    //create buffer to store information we are going to send back
    // char *response;
    
    //extracting information from the message
    uint8_t nameLength = *(uint8_t *)(&message[0]); //room name length

    char *name = malloc(sizeof(message[1])); //actual name
    sprintf(name, "%s", &message[1]); //DANGER: null byte terminator needed in order to know end of the name? 
    name[nameLength] = '\0';

    uint8_t passwordLength = *(uint8_t *)(&message[1 + nameLength]);
    char *password = malloc((size_t)(passwordLength + 1)); //actual password
    if(passwordLength > 0) 
        sprintf(password, "%s", &message[1 + nameLength + 1]); //DANGER: null byte terminator needed in order to know end of the name?
    else 
        sprintf(password, "%s", ""); //will insert the null terminating byte in there

    //initalize message here (this will be populated below)
    char *response;
    int responseLength;

    //is the user already in this room? 
    if(currUser->userRoom != NULL && strcmp(currUser->userRoom->name, name) == 0) {
        //construct response message
        responseLength = 74;
        response = malloc(responseLength);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(0x42);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 1;
        sprintf(&response[8], "%s", "You fail to bend space and time to reenter where you already are.");
        responseLength--; //ignore null terminating byte

        sendMessage(clntSock, response, responseLength);
        free(response);
        return;
    }

    //------- altering users global array, lock the mutex! --------
    pthread_mutex_lock(&joinLeaveNickLock);

    //if the user is currently in a room? 
    if(currUser->userRoom != NULL) {

        //iterate through the room's list of users and for the index corresponding to this user, set it to NULL
        int currNumOfRoomUsers = currUser->userRoom->numUsers;
        for(int i = 0; i < MAX_USERS; i++) {
            if(currNumOfRoomUsers == 0) {
                fprintf(stderr, "ERROR: attempted to remove a user from a room that did not hold that user\n");
                return;
            }

            if(currUser->userRoom->users[i] != NULL) {
                currNumOfRoomUsers--;
                if(strcmp(currUser->userRoom->users[i]->name, currUser->name) == 0) {
                    currUser->userRoom->users[i] = NULL;
                    currUser->userRoom->numUsers--;
                    break;
                }
            }
        }
        
        //if the room's number of users is 0? delete this room with the function
        if(currUser->userRoom->numUsers == 0) deleteRoom(currUser->userRoom);

        //set the users reference to that room to be NULL (optional)
        currUser->userRoom = NULL;
    }

    //does the desired room already exist? 
    int indexOfDesiredRoom = -1;
    int currNumOfRooms = numOfRooms;
    for(int i = 0; i < MAX_ROOMS; i++) {
        if(currNumOfRooms == 0) break;
        if(allRooms[i] != NULL) {
            currNumOfRooms--;
            if(strcmp(allRooms[i]->name, name) == 0) {
                indexOfDesiredRoom = i;
                break;
            }
        }
    }

    // printf("indexOfDesiredRoom is: %d\n", indexOfDesiredRoom);

    //if the room already exists? 
    int isAllowedToJoinRoom = 1;
    if(indexOfDesiredRoom != -1) {
        //does the password for the room match? then continue; but if it doesnt match then we cant put the user into this room
        if(strcmp(allRooms[indexOfDesiredRoom]->password, password) == 0) {
            //nothing to be done here
        } else { //if the password does not match?
            isAllowedToJoinRoom = 0;
        }
    } else { //if the room does not already exist? create it

        //create the room ONLY, REMEMBER to assin indexOfDesiredRoom to the index in which your putting the newly created room
        indexOfDesiredRoom = getNextOpenRoomIndex();
        if(indexOfDesiredRoom == -1) {
            fprintf(stderr, "ERROR: cannot fit another user into this room\n");
            isAllowedToJoinRoom = 0;
        }

        allRooms[indexOfDesiredRoom] = malloc(sizeof(Room)); //put this room officially on the server

        allRooms[indexOfDesiredRoom]->name = malloc(strlen(name) + 1); //assign this room its name
        sprintf(allRooms[indexOfDesiredRoom]->name, "%s", name);

        allRooms[indexOfDesiredRoom]->password = malloc(strlen(password) + 1); //assign this room its password
        sprintf(allRooms[indexOfDesiredRoom]->password, "%s", password);

        allRooms[indexOfDesiredRoom]->numUsers=0; //assign this room no users

        for(int i = 0; i < MAX_USERS; i++) //initialize each user pointer in this room to NULL (just in case for garbage values)
            allRooms[indexOfDesiredRoom]->users[i] = NULL;
        
        numOfRooms++;
    }

    //if isAllowedToJoinRoom == 1 then 
    if(isAllowedToJoinRoom == 1) {
        //link the user to this room
        currUser->userRoom = allRooms[indexOfDesiredRoom];

        //add this user into the list of users in that room
        int inserted = 0;
        for(int i = 0; i < MAX_USERS; i++) {
            if(currUser->userRoom->users[i] == NULL) {
                currUser->userRoom->users[i] = currUser;
                inserted = 1;
                break;
            }
        }
        if(inserted == 0) {
            fprintf(stderr, "ERROR: not enough room in room to fit user in join function\n");
            return;
        }

        //increment the num of users in that room
        currUser->userRoom->numUsers++;

        //construct response message
        responseLength = 8;
        response = malloc(responseLength);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(1);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 0;

    } else if(isAllowedToJoinRoom == 0){ //else if its not allowed to join the room? 
        //construct response message
        responseLength = 44;
        response = malloc(responseLength);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(36);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 1;
        sprintf(&response[8], "%s", "Wrong password. You shall not pass!");
        responseLength--; //ignore null terminating byte
    } else {
        fprintf(stderr, "ERROR: unexpected value for isAllowedToJoinRoom, was %d\n", isAllowedToJoinRoom);
        return;
    }

    pthread_mutex_unlock(&joinLeaveNickLock);
    //------------------ done! unlock mutex! ----------------------

    sendMessage(clntSock, response, responseLength);
    free(response);
    free(name);
    free(password);
}

void removeUserFromRoom(struct user *currUser) {
    //if the user is in a room right now? we have to remove the user from the room
    if(currUser->userRoom != NULL) {

        //iterate through the room's list of users and for the index corresponding to this user, set it to NULL
        int currNumOfRoomUsers = currUser->userRoom->numUsers;
        for(int i = 0; i < MAX_USERS; i++) {
            if(currNumOfRoomUsers == 0) {
                fprintf(stderr, "ERROR: attempted to remove a user from a room that did not hold that user\n");
                return;
            }

            if(currUser->userRoom->users[i] != NULL) {
                currNumOfRoomUsers--;
                if(strcmp(currUser->userRoom->users[i]->name, currUser->name) == 0) {
                    currUser->userRoom->users[i] = NULL;
                    currUser->userRoom->numUsers--;
                    break;
                }
            }
        }
        
        //if the room's number of users is 0? delete this room with the function
        if(currUser->userRoom->numUsers == 0) deleteRoom(currUser->userRoom);

        //set the users reference to that room to be NULL (optional)
        currUser->userRoom = NULL;
    }
}

void deleteUser(struct user *currUser, int index) {
    removeUserFromRoom(currUser); //remove user from room if they are in a room

    if(strcmp(allUsers[index]->name, currUser->name) == 0) {
        free(currUser->name); //works now!
        allUsers[index] = NULL;
    } else {
        fprintf(stderr, "ERROR: attempted to delete user with the incorrect index in deleteUser function\n"); //shouldnt happen
        return;
    }

    //decrement the total number of users on the server
    numOfUsers--;
}

int handleLeave(int clntSock, struct user *currUser, int index) {
    int willDisconnect = 0;
    if(currUser->userRoom != NULL) {
        removeUserFromRoom(currUser);
    } else { 
        deleteUser(currUser, index);
        willDisconnect = 1; //will disconnect this user
    }
    
    //construct response message
    int responseLength = 8;
    char *response = malloc(responseLength);
    *(uint16_t *)&response[0] = htons(0x0417);  
    *(uint32_t *)&response[2] = htonl(1);
    *(uint8_t *)&response[6] = 0xfe;
    *(uint8_t *)&response[7] = 0;

    //send message
    sendMessage(clntSock, response, responseLength);

    //free memory
    free(response);
    
    //return correct value
    return willDisconnect;
}

void handleListUsers(int clntSock, struct user *currUser) {
    //------- altering users global array, lock the mutex! --------
    pthread_mutex_lock(&joinLeaveNickLock);

    struct user **currUsers = (currUser->userRoom == NULL)? allUsers : currUser->userRoom->users;
    int currNumOfUsers = (currUser->userRoom == NULL)? numOfUsers : currUser->userRoom->numUsers;
    int reset = currNumOfUsers;

    //count total bytes needed for payload
    int payloadLength = 1;
    for(int i = 0; i < MAX_USERS; i++) {
        if(currNumOfUsers == 0) break;
        if(currUsers[i] != NULL) {
            currNumOfUsers--;
            payloadLength += (1 + strlen(currUsers[i]->name));
        }
    }
    // payloadLength++; //just to make room for the null terminator so we dont leak into other memory (will remove this 1 later)

    //construct response
    int responseLength = (7 + payloadLength);
    char *response = malloc(responseLength + 1);
    *(uint16_t *)&response[0] = htons(0x0417);  
    *(uint32_t *)&response[2] = htonl(payloadLength);
    *(uint8_t *)&response[6] = 0xfe;
    *(uint8_t *)&response[7] = 0;

    int messageIndex = 8;
    currNumOfUsers = reset;
    for(int i = 0; i < MAX_USERS; i++) {
        if(currNumOfUsers == 0) break;    
        if(currUsers[i] != NULL) {
            currNumOfUsers--;
            uint8_t nameLength = strlen(currUsers[i]->name);

            // *(uint8_t *)&response[messageIndex++] = nameIndex++;
            *(uint8_t *)&response[messageIndex++] = nameLength;
            sprintf(&response[messageIndex], "%s", currUsers[i]->name);
            messageIndex += nameLength;
        }
    }
    // responseLength--; //ignore null terminator

    pthread_mutex_unlock(&joinLeaveNickLock);
    //------------------ done! unlock mutex! ----------------------

    //send response
    sendMessage(clntSock, response, responseLength);

    //free memory
    free(response);
}

void handleUnknown(int clntSock) {
    //construct response message
    int responseLength = 61;
    char *response = malloc(responseLength+1);
    *(uint16_t *)&response[0] = htons(0x0417);  
    *(uint32_t *)&response[2] = htonl(0x36);
    *(uint8_t *)&response[6] = 0xfe;
    *(uint8_t *)&response[7] = 1;
    sprintf(&response[8], "%s", "You shout into the void and hear nothing but silence.");

    sendMessage(clntSock, response, responseLength);

    free(response);
}

void handleListRooms(int clntSock) {
    //------- altering users global array, lock the mutex! --------
    pthread_mutex_lock(&joinLeaveNickLock);

    //make an array of all existing room names
    char *tempRooms[numOfRooms];
    int currNumOfRooms = numOfRooms, index = 0, payloadLength = 1;
    for(int i = 0; i < MAX_ROOMS; i++) {
        if(currNumOfRooms == 0) break;
        if(allRooms[i] != NULL) {
            currNumOfRooms--;
            int nameLength = strlen(allRooms[i]->name);

            tempRooms[index] = malloc(nameLength + 1);
            sprintf(tempRooms[index++], "%s", allRooms[i]->name);
            payloadLength += (1 + nameLength);
        } 
    }

    //construct response
    int responseLength = (7 + payloadLength);
    char *response = malloc(responseLength + 1);
    *(uint16_t *)&response[0] = htons(0x0417);  
    *(uint32_t *)&response[2] = htonl(payloadLength);
    *(uint8_t *)&response[6] = 0xfe;
    *(uint8_t *)&response[7] = 0;

    int messageIndex = 8;
    for(int i = 0; i < numOfRooms; i++) {
        int minNameIndex = -1;

        currNumOfRooms = numOfRooms - i;
        for(int j = 0; j < numOfRooms; j++) {
            if(currNumOfRooms == 0) break;
            if(tempRooms[j] != NULL) {
                currNumOfRooms--;
                if(minNameIndex == -1 || strcmp(tempRooms[minNameIndex], tempRooms[j]) > 0)
                    minNameIndex = j;
            }
        }
        
        //inserting into the response
        int nameLength = strlen(tempRooms[minNameIndex]);
        // *(uint8_t *)&response[messageIndex++] = i;
        *(uint8_t *)&response[messageIndex++] = nameLength;
        sprintf(&response[messageIndex], "%s", tempRooms[minNameIndex]);
        messageIndex += nameLength;

        free(tempRooms[minNameIndex]);
        tempRooms[minNameIndex] = NULL; //erase from temp array of room names
    }

    //if there are no rooms
    if(numOfRooms == 0) {
        // responseLength++;
        // *(uint32_t *)&response[2] = htonl(1);
        // *(uint8_t *)&response[7] = 0;
    }

    //sending message
    sendMessage(clntSock, response, responseLength);
    free(response);

    pthread_mutex_unlock(&joinLeaveNickLock);
    //------------------ done! unlock mutex! ----------------------
}

void handleMsg(struct user *currUser, char *message) {
    //extracting information from message
    uint8_t nameLength = *(uint8_t *)(&message[0]);
    char *name = malloc(nameLength + 1); //actual name
    sprintf(name, "%s", &message[1]); //DANGER: null byte terminator needed in order to know end of the name? 
    name[nameLength] = '\0'; //shouldnt be needed

    uint8_t messageLength = *(uint8_t *)(&message[1+nameLength+1]);
    char *messageToUser = malloc(messageLength+1);
    sprintf(messageToUser, "%s", &message[1+nameLength+1+1]);

    // printf("message length is: %u\n", *(uint8_t *)(&message[1+nameLength+1]));

    //------- altering users global array, lock the mutex! --------
    pthread_mutex_lock(&joinLeaveNickLock); 
    //need to lock mutex because in the middle of sending a message, the other user could leave
    //and then cause us to send a message to a null pointer, which would crash the server

    //does the desired user exist? (you can send a message to yourself)
    struct user *desiredUser = NULL;
    int currNumOfUsers = numOfUsers;
    for(int i = 0; i < MAX_USERS; i++) {
        if(currNumOfUsers == 0) break; //user does not exist
        if(allUsers[i] != NULL) {
            currNumOfUsers--;
            if(strcmp(allUsers[i]->name, name) == 0) {
                desiredUser = allUsers[i];
                break;
            }
        }
    }

    int responseLength;
    char *response;

    //if the desired user exists
    if(desiredUser != NULL) {
        //construct message for sending user
        responseLength = 8;
        response = malloc(responseLength);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(1);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 0;

        //send message to sending user
        sendMessage(currUser->clntSock, response, responseLength);

        //construct message for recieving user
        int sendingNameLength = strlen(currUser->name);
        int payloadLength = (1 + sendingNameLength + 1 + 1 + messageLength);
        responseLength = (7 + payloadLength);
        response = malloc(responseLength + 1); //1 byte for the null terminating
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(payloadLength);
        *(uint8_t *)&response[6] = 0x1c;
        *(uint8_t *)&response[7] = sendingNameLength;
        sprintf(&response[8], "%s", currUser->name);
        *(uint8_t *)&response[8 + sendingNameLength] = 0;
        *(uint8_t *)&response[8 + sendingNameLength + 1] = messageLength;
        sprintf(&response[8 + sendingNameLength + 1 + 1], "%s", messageToUser);

        // printf("%u : %u : %u : %u : %s : %u : %u : %s\n", 
        // ntohs(*(uint16_t *)&response[0]),
        // ntohl(*(uint32_t *)&response[2]),
        // *(uint8_t *)&response[6],
        // *(uint8_t *)&response[7],
        // currUser->name,
        // *(uint8_t *)&response[8 + sendingNameLength],
        // *(uint8_t *)&response[8 + sendingNameLength + 1],
        // messageToUser);

        //send message to recieving user
        sendMessage(desiredUser->clntSock, response, responseLength);

    } else {
    //if the desired user does NOT exist

        //construct message for sending user
        responseLength = 23;
        response = malloc(responseLength + 1);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(16);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 1;
        sprintf(&response[8], "%s", "Nick not found.");

        //send message to sending user
        sendMessage(currUser->clntSock, response, responseLength);
    }

    pthread_mutex_unlock(&joinLeaveNickLock);
    //------------------ done! unlock mutex! ----------------------

    free(name);
    free(messageToUser);
    free(response);
}

void handleNick(struct user *currUser, char *message) {
    //extract information from the message
    uint8_t nameLength = *(uint8_t *)(&message[0]);
    char *newName = malloc(nameLength + 1);
    sprintf(newName, "%s", &message[1]);
    newName[nameLength] = '\0'; //terminate the newname with null

    //does this name already exist?
    int nameAlreadyUsed = 0;
    int currNumOfUsers = numOfUsers;
    for(int i = 0; i < MAX_USERS; i++) {
        if(currNumOfUsers == 0) break;
        if(allUsers[i] != NULL) {
            currNumOfUsers--;
            if(strcmp(allUsers[i]->name, newName) == 0) {
                nameAlreadyUsed = 1;
                break;
            }
        }
    }

    //prepare to construct response
    int responseLength;
    char *response;

    //if the name is not being used
    if(nameAlreadyUsed == 0) {
        //change currUsers name to desired name
        sprintf(currUser->name, "%s", newName);

        //construct response
        responseLength = 8;
        response = malloc(responseLength);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(1);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 0;

        //send message
        sendMessage(currUser->clntSock, response, responseLength);
    } else {
    //if the name is already being used

        //construct response
        int payloadLength = (1 + 33);
        responseLength = (2 + 4 + 1 + payloadLength);
        response = malloc(responseLength + 1);
        *(uint16_t *)&response[0] = htons(0x0417);  
        *(uint32_t *)&response[2] = htonl(34);
        *(uint8_t *)&response[6] = 0xfe;
        *(uint8_t *)&response[7] = 1;
        sprintf(&response[8], "%s", "Someone already nicked that nick.");

        //send message
        sendMessage(currUser->clntSock, response, responseLength);
    }

    free(response);
    free(newName);
    // printf("%u %s\n", nameLength, newName);
}

void *handleTCPClient(void *info) {
    int clntSock = *((int *)info);
    struct user *currUser;
    int indexInAllUsersArray;

    char *preMessage = malloc(7);
    while(1) {
        //waiting for client to send a message...

        //incoming message! reading in pre message (aka first 7 bytes)
        int bytesRead = recv(clntSock, preMessage, 7, 0);

        //if a timeout occurs or if the client decides to leave
        if(bytesRead < 0 || bytesRead == 0) {
            //timeout occured because client took too long to send a message
            // if(bytesRead < 0) printf("timeout occured\n");
            // else if(bytesRead == 0) printf("user quit\n");

            //------- altering users global array, lock the mutex! --------
            pthread_mutex_lock(&joinLeaveNickLock);
            
            deleteUser(currUser, indexInAllUsersArray); //user must have been created by the time this line is called

            pthread_mutex_unlock(&joinLeaveNickLock);
            //------------------ done! unlock mutex! ----------------------
            break;
        } else if(bytesRead != 7) {
            fprintf(stderr, "recieved unexpected number of bytes in handleTCPClient 1: %d bytes\n", bytesRead);
            break;
        }

        //making sure the first 2 bytes were 0x417
        if(!(ntohs(*(uint16_t *)(&preMessage[0])) == 0x417)) {
            //if message is not preceded with 0x417 then we ignore it
            fprintf(stderr, "recieved message but it did not precede with 0x417\n");
            continue;
        }

        //extract further information from premessage
        uint32_t payloadLength = ntohl(*(uint32_t *)(&preMessage[2])); //length of entire payload
        // printf("payloadLength: %u\n", payloadLength);
        uint8_t opCode = *(uint16_t *)(&preMessage[6]); //operation code (what the user wants us to do)

        //now that we know how long the payload will be, we make a buffer of that length
        char *message;

        if(payloadLength > 0) {
            message = malloc(payloadLength);

            //read in rest of message
            bytesRead = recv(clntSock, message, payloadLength, 0);
            if(bytesRead < 0) {
                //timeout occured
                fprintf(stderr, "ERROR: timeout occured at unexpected location\n");
                break;
            } else if((uint32_t)bytesRead != payloadLength) {
                fprintf(stderr, "recieved unexpected number of bytes in handleTCPClient 2: %d bytes\n", bytesRead);
                break;
            }
        }
        
        int willDisconnect = 0;

        //switch on opCode (which command are we dealing with)
        switch(opCode) {
            case 0xff:
                ;
                indexInAllUsersArray = handleConnect(clntSock, message); //will get -1 if that message is unrecognized
                currUser = allUsers[indexInAllUsersArray];
                break;
            case 0x17:
                handleJoin(clntSock, message, currUser);
                break;
            case 0x18:
                willDisconnect = handleLeave(clntSock, currUser, indexInAllUsersArray);
                break;
            case 0x1a:
                handleListUsers(clntSock, currUser);
                break;
            case 0x19:
                handleListRooms(clntSock);
                break;
            case 0x1c:
                handleMsg(currUser, message);
                break;
            case 0x1b:
                handleNick(currUser, message);
                break;
            default:
                handleUnknown(clntSock);
                break;
        }

        //free message
        if(payloadLength > 0) free(message);

        //if we are to disconnect
        if(willDisconnect == 1) break;
    }

    free(preMessage);
    close(clntSock);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    // struct server_arguments args = server_parseopt(argc, argv);
    // int port = atoi(argv[2]);
    int port = atoi(argv[argc-1]); 


    //create socket
    int servSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(servSock < 0) {
        fprintf(stderr, "ERROR: server creating sock - failed");
        exit(0);
    }

    //initialize users and rooms array to null
    for(int i = 0; i < MAX_USERS; i++) 
        allUsers[i] = NULL;
    for(int i = 0; i < MAX_ROOMS; i++)
        allRooms[i] = NULL;

    //initialize number of users to be 0
    numOfUsers = 0;
    numOfRooms = 0;

    //construct the server address structure
	struct sockaddr_in servAddr; 
	memset(&servAddr, 0, sizeof(servAddr)); 
	servAddr.sin_family = AF_INET;
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servAddr.sin_port = htons(port);

    //binding to the local address
    if(bind(servSock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0) {
        fprintf(stderr, "ERROR: server binding to local address - failed");
        exit(1);
    }

    //initiate listening on that socket
    //i just did 10 as the max connections to the socket
    if(listen(servSock, 10) < 0) {
        fprintf(stderr, "ERROR: server listening for incoming connections failed - failed");
        exit(1);
    }

    int numThreads = 0;
    pthread_t tid[MAX_USERS]; //will eventually run out...

    for(;;) {
        struct sockaddr_in clntAddr;

        //set length of client address structure
        socklen_t clntAddrLen = sizeof(clntAddr);

        //waiting for new client to connect
        numThreads++;
        int clntSock = accept(servSock, (struct sockaddr *) &clntAddr, &clntAddrLen);

        //first set the socket to have the timeout
        struct timeval tv;
        tv.tv_sec = TIMEOUT; 
        tv.tv_usec = 0;
        setsockopt(clntSock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
        
        //checking if clnt socket was accepted succesfully
        if(clntSock < 0) {
            fprintf(stderr, "ERROR: server connecting to client - failed");
            exit(1);
        }

        // create thread for this client
        if(pthread_create(&tid[numThreads++], NULL, handleTCPClient, &clntSock) != 0) {
            fprintf(stderr, "ERROR: server couldnt make thread to connect with client - failure");
            exit(1);
        }

        //loop back to wait for more clients
        // SHOULD NOT NEED TO CALL close(clntSock); HERE
    }

    return 0;
}
