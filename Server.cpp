#undef UNICODE

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>
#include <fstream>

// Need to link with Ws2_32.lib
#pragma comment (lib, "Ws2_32.lib")
// #pragma comment (lib, "Mswsock.lib")

#define DEFAULT_BUFLEN 512
#define DEFAULT_PORT "27015"
#define MAX_CONNECTION_COUNT 200

struct MessageHeader {
	int type;
	int length;
};

enum MESSAGETYPE {
	GPS = 0,
	UPLOAD,
	DOWNLOAD
};

struct FileName {
	int fnamelen;
	//char *f_name;
};

struct GPSMSG {
	float value;
};

volatile bool doistop = false;

DWORD WINAPI ReceiveRequest(void *pParam);

int  main(void)
{
	WSADATA wsaData;
	int iResult;

	SOCKET ListenSocket = INVALID_SOCKET;
	SOCKET ClientSocket = INVALID_SOCKET;

	struct addrinfo *result = NULL;
	struct addrinfo hints;
	HANDLE threads[MAX_CONNECTION_COUNT];

	
	int threadcount = 0;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		return 1;
	}

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	iResult = getaddrinfo("10.139.57.20", DEFAULT_PORT, &hints, &result);
	if (iResult != 0) {
		printf("getaddrinfo failed with error: %d\n", iResult);
		WSACleanup();
		return 1;
	}

	// Create a SOCKET for connecting to server
	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET) {
		printf("socket failed with error: %ld\n", WSAGetLastError());
		freeaddrinfo(result);
		WSACleanup();
		return 1;
	}

	// Setup the TCP listening socket
	iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		printf("bind failed with error: %d\n", WSAGetLastError());
		freeaddrinfo(result);
		closesocket(ListenSocket);
		WSACleanup();
		return 1;
	}

	freeaddrinfo(result);

	iResult = listen(ListenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR) {
		printf("listen failed with error: %d\n", WSAGetLastError());
		closesocket(ListenSocket);
		WSACleanup();
		return 1;
	}

	while (1) {

		// Accept a client socket
		printf("\nListening for connections...\n");
		ClientSocket = accept(ListenSocket, NULL, NULL);

		if (ClientSocket == INVALID_SOCKET) {
			printf("accept failed with error: %d\n", WSAGetLastError());
			continue;
		}
		threads[threadcount++] = CreateThread(NULL, 0, ReceiveRequest, (void*)ClientSocket, 0, NULL);
	}

	doistop = true;
	WaitForMultipleObjects(MAX_CONNECTION_COUNT, threads, TRUE, INFINITE);

	return 0;
}

DWORD WINAPI ReceiveRequest (void *pParam)
{
	int iResult;
	int totalreceived = 0;
	char *recvbuf;
	int recvbuflen = sizeof (MessageHeader);
	int type;
	int payloadlength = 0;
	bool isheaderreceived = false;
	int fnamelen = 0;
	char *file_name = NULL;
	FILE *File;
	SOCKET ClientSocket = (SOCKET)pParam;
	MessageHeader *header = NULL;

	recvbuf = (char*)calloc(1, sizeof(MessageHeader));
	// Receive until the peer shuts down the connection
	while (!doistop) {
		iResult = recv(ClientSocket, recvbuf + totalreceived, recvbuflen - totalreceived, 0);
		if (iResult > 0) {
			if (!isheaderreceived) {
				totalreceived += iResult;
				if (totalreceived != sizeof(MessageHeader)) continue;

				header = (MessageHeader*)recvbuf;
				payloadlength = recvbuflen = header->length;
				type = header->type;
				free(recvbuf);
				recvbuf = (char*)calloc(1,payloadlength);
				isheaderreceived = true;
				totalreceived = 0;
			}
			else {
				totalreceived += iResult;
				if (totalreceived != payloadlength) continue;

				switch (type) {
					case GPS:		printf("Received GPS data: %f\n", ((GPSMSG*)recvbuf)->value);									
									break;
					case UPLOAD:	fnamelen = ((FileName*)recvbuf)->fnamelen;
									file_name = (char*)calloc(1, fnamelen + 1);
									memcpy(file_name, recvbuf + sizeof(int), fnamelen);
									fopen_s(&File, file_name, "wb");
									free(file_name);
									if (!File) {
										printf("Error while writing the file at Server\n");
										break;
									}
									fwrite(recvbuf+sizeof(int)+fnamelen, sizeof(char), payloadlength-sizeof(int)-fnamelen, File);
									fclose(File);
									break;
					case DOWNLOAD:	//Read header for file name
									fnamelen = ((FileName*)recvbuf)->fnamelen;
									file_name = (char*)calloc(1,fnamelen + 1);
									memcpy(file_name, recvbuf + sizeof(int), fnamelen);
									char *Buffer;
									unsigned long Size;
									//make file name dynamic
									fopen_s(&File, file_name, "rb");
									free(file_name);
									if (!File) {
										printf("Error while reading the file at Server\n");
										break;
									}

									fseek(File, 0, SEEK_END);
									Size = ftell(File);
									fseek(File, 0, SEEK_SET);
									Buffer = (char*)calloc(1, sizeof(MessageHeader) + Size);
									fread(Buffer+sizeof(MessageHeader), Size, 1, File);
									fclose(File);
									((MessageHeader*)Buffer)->type = DOWNLOAD;
									((MessageHeader*)Buffer)->length = Size;
									int sendbuflen = Size + sizeof(MessageHeader);
									int totalsent = 0;
									while (1) {
										// Send an initial buffer
										iResult = send(ClientSocket, Buffer + totalsent, sendbuflen - totalsent, 0);
										if (iResult == SOCKET_ERROR) {
											printf("Failed while serving download request with error: %d\n", WSAGetLastError());
											closesocket(ClientSocket);
											free(recvbuf);
											free(Buffer);
											return 0;
										}
										totalsent += iResult;
										if (totalsent != sendbuflen) continue;
										printf("Sent download data\n");
										free(Buffer);
										break;
									}
									break;
					//default:		printf("Received unknown header\n");
				}

				free(recvbuf);
			}

		} 
		else if (iResult == 0) {
			printf("Connection closing...\n");
			break;
		} 
		else {
			printf("recv failed with error: %d\n", WSAGetLastError());
			closesocket(ClientSocket);
			break;
		}

	}

	// shutdown the connection since we're done
	iResult = shutdown(ClientSocket, SD_SEND);
	if (iResult == SOCKET_ERROR) {
		printf("shutdown failed with error: %d\n", WSAGetLastError());
	}

	// cleanup
	closesocket(ClientSocket);

	return 0;
}
