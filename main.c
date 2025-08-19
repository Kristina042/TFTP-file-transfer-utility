//
//TCP command line file transfer program
//

#include <stdio.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include "tmr.h"
#include <stdbool.h>

#ifdef _WIN32
	#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <fcntl.h>
	#include <errno.h>
	#include <signal.h>
	#include <unistd.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netdb.h>
#endif

#ifndef _WIN32
typedef int SOCKET;
#define INVALID_SOCKET -1
#endif

#define PROT_MAX_DATA 			512
#define MAX_TX_BUFF				600
#define MAX_RX_BUFF				2048
#define MAX_MODE_BUFF			12

#define ACK_TIMEOUT_SECS			3
#define SEND_DATA_TIMEOUT_SEC		3
#define PROGRESS_TMR_SEC			3
#define DEF_TFTP_PORT				69


//reading packet machine states
typedef enum
{
	READ_OPT_CODE_1,		//optcode low byte
	READ_OPT_CODE_2,		//optcode high byte
	READ_FILENAME,
	READ_MODE,
	READ_DATA,
	READ_BLOCK_NUM_1,
	READ_BLOCK_NUM_2,
	READ_ERR_CODE_1,
	READ_ERR_CODE_2,
	READ_ERR_MESSAGE
} prot_st_t;

typedef enum
{
	TFTP_RRQ = 1,		// Read Request, getfile
	TFTP_WRQ  = 2,		// Write Request, putfile
	TFTP_DATA = 3,		// Data Packet
	TFTP_ACK = 4,		// Acknowledgment
	TFTP_ERROR = 5		// Error Packet
} tftp_opcode_t;

//recieve protocol structure
typedef struct
{
	uint8_t state;

	uint16_t optcode;
	uint16_t blocknum;
	uint16_t errCode;

	uint16_t rxLen;

	uint8_t dataBuf[PROT_MAX_DATA];
	uint8_t filename[PROT_MAX_DATA];
	uint8_t mode[MAX_MODE_BUFF];
	uint8_t errMessage[PROT_MAX_DATA];

	int isLastDataBlock; // 1 if id data is less than 512
} prot_frame_info_t;

//client session
typedef struct
{
	SOCKET clientSock;

	const char* remoteIpStr;
	uint16_t remotePort;	//69, port to establish session
	uint16_t svrPort;		//port server chooses

	int state;

	int isFirstDataBlock;

	tick_timer_t tmr1; //timer waiting for acks or data
	tick_timer_t tmr2; //timer to print progress

	int num_retrans_tries;

	uint16_t blockNum;
	uint16_t nextExpectedBlockNum;

	prot_frame_info_t rxInfo;

	FILE * pFile;
	const char* filename;

	uint16_t lastTxPort;

	// file transmit buffer and lenght variables
	uint8_t txBuf[MAX_TX_BUFF];
	uint16_t txLen;
} client_session_t;

//client session
typedef struct
{
	SOCKET serverSock;

	const char* remoteIpStr;
	uint16_t client_Port;	//client port that server will retrieve from recvfrom() function
	const char* client_ip;	//client IP port that server will retrieve from recvfrom() function

	// timers
	tick_timer_t tmr1; 		//timer waiting for acks or data
	tick_timer_t tmr2; 		//timer to print progress

	int num_retrans_tries;
	int state;

	uint16_t blockNum;
	uint16_t nextExpectedBlockNum;

	prot_frame_info_t rxInfo;

	FILE * pFile;
	const char* filename;

	uint16_t lastTxPort;

	// file transmit buffer and lenght variables
	uint8_t txBuf[MAX_TX_BUFF];
	uint16_t txLen;
} server_session_t;

//client protocol machine states
typedef enum
{
	CL_ST_GETFILE_RXDATA,				// Receving normal data (GETFILE session)
	CL_ST_PUTFILE_TXDATA,				// Sending normal data (PUTFILE session)
}cl_st_t;

//server protocol machine states
typedef enum
{
	SVR_ST_WAIT_FIST_REQUEST,		//wait for getfile or putfile request
	SVR_ST_GETFILE_TXDATA,				// Sending normal data (GETFILE session)
	SVR_ST_PUTFILE_RXDATA,				// Recieving normal data (PUTFILE session)
}svr_st_t;

//FSM client events
typedef enum
{
	EV_CL_TIMEOUT,			// timeout
	EV_CL_PDU_RX,			// Full protocol data unit received
} cl_evt_t;

//FSM server events
typedef enum
{
	EV_SVR_TIMEOUT,				// timeout
	EV_SVR_PDU_RX,				// Full protocol data unit received
} svr_evt_t;


static client_session_t clientCtx;
static server_session_t serverCtx;

static int gDone = 0;
static int gFsmDebugOn = 0;
static int gDebugDropPacket = 0;
static int gDebugDropAllPks = 0;
static int gMaxNumRetransTries = 3;
static uint16_t gSrvPort = DEF_TFTP_PORT;

//detection of ctrl+c
#ifdef _WIN32
	BOOL WINAPI signal_handler(DWORD dwCtrlType)
	{
		switch (dwCtrlType)
		{
			case CTRL_C_EVENT:
			case CTRL_BREAK_EVENT:
			case CTRL_CLOSE_EVENT:
				gDone = 1;
				printf("detected Ctrl-C, exiting...\n");
				return TRUE;
		}

		return FALSE;
	}
#else
	static void signal_handler(int sig)
	{
		switch (sig)
		{
			case SIGTERM:
			case SIGHUP:
			case SIGINT:
			case SIGQUIT:
				gDone= 1;
				printf("detected Ctrl-C, exiting...\n");
				break;
			}
	}
#endif

//gets client state name
//state - client state
//name - pointer to string buffer that receives the name
static void client_get_state_name(int state, char *name)
{
	switch (state)
	{
		case CL_ST_GETFILE_RXDATA:
			strcpy(name, "CL_ST_GETFILE_RXDATA");
			break;

		case CL_ST_PUTFILE_TXDATA:
			strcpy(name, "CL_ST_PUTFILE_TXDATA");
			break;

		default:
			strcpy(name, "UNKNOWN_ST");
			break;
	}
}

//gets server state name
//state - server state
//name - pointer to string buffer that receives the name
static void server_get_state_name(int state, char *name)
{
	switch (state)
	{
		case SVR_ST_GETFILE_TXDATA:
			strcpy(name, "SVR_ST_GETFILE_TXDATA");
			break;

		case SVR_ST_PUTFILE_RXDATA:
			strcpy(name, "SVR_ST_PUTFILE_RXDATA");
			break;

		case SVR_ST_WAIT_FIST_REQUEST:
			strcpy(name, "SVR_ST_WAIT_FIST_REQUEST");
			break;

		default:
			strcpy(name, "UNKNOWN_ST");
			break;
	}
}

//gets server event name
//event - server event
//name - pointer to string buffer that receives the name
static void server_get_event_name(int event, char *name)
{
	switch (event)
	{
		case EV_SVR_TIMEOUT:
			strcpy(name, "EV_SVR_TIMEOUT");
			break;

		case EV_SVR_PDU_RX:
			strcpy(name, "EV_SVR_PDU_RX");
			break;

		default:
			strcpy(name, "UNKNOWN_EV");
			break;
	}
}

//gets client event name
//event - client event
//name - pointer to string buffer that receives the name
static void client_get_event_name(int event, char *name)
{
	switch (event)
	{
		case EV_CL_TIMEOUT:
			strcpy(name, "EV_CL_TIMEOUT");
			break;

		case EV_CL_PDU_RX:
			strcpy(name, "EV_CL_PDU_RX");
			break;

		default:
			strcpy(name, "UNKNOWN_EV");
			break;
	}
}

//changes to a new client state
//ctx - pointer to client session context
//newState - state to switch into
static void client_change_state(client_session_t *ctx, int newState)
{
	char stateNameOld[64];
	char stateNameNew[64];

	if (gFsmDebugOn)
	{
		client_get_state_name(ctx->state, stateNameOld);
		client_get_state_name(newState, stateNameNew);

		printf("%s -> %s\n", stateNameOld, stateNameNew);
	}

	//change to new state
	ctx->state = newState;
}

//changes to a new server state
//ctx - pointer to server session context
//newState - state to switch into
static void server_change_state(server_session_t *ctx, int newState)
{
	char stateNameOld[64];
	char stateNameNew[64];

	if (gFsmDebugOn)
	{
		server_get_state_name(ctx->state, stateNameOld);
		server_get_state_name(newState, stateNameNew);

		printf("%s -> %s\n", stateNameOld, stateNameNew);
	}

	//change to new state
	ctx->state = newState;
}

//send a packet buffer to the server
//ctx - pointer to client session context
//isReTransmit: 1 - is a reTransmission packet, 0 - is a regulat packet
// 0 = failed, 1=success
static int cl_send_packet_buffer(client_session_t *ctx, int isReTransmit)
{
	int rc;
	struct sockaddr_in Addr;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = (isReTransmit) ? ctx->lastTxPort : htons(ctx->svrPort);
	Addr.sin_addr.s_addr = inet_addr(ctx->remoteIpStr);

	ctx->lastTxPort = Addr.sin_port;

	#ifdef _WIN32
		rc = sendto(ctx->clientSock, (const char*)ctx->txBuf ,(size_t)ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->clientSock, ctx->txBuf, (size_t)ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc < 0)
	{
		#ifdef _WIN32
			int err = WSAGetLastError();
			printf("sendto failed with error code: %d\n", err);
		#else

			printf("sendto returns error: rc=%d\n", rc);
			printf("error code:%d (%s)\n", errno, strerror(errno));
		#endif
		return 0;
	}
	return 1;
}

//send a packet buffer to the server
//ctx - pointer to client session context
//isReTransmit: 1 - is a reTransmission packet, 0 - is a regulat packet
// 0 = failed, 1=success
static int svr_send_packet_buffer(server_session_t *ctx, int isReTransmit)
{
	int rc;
	struct sockaddr_in Addr;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = (isReTransmit) ? ctx->lastTxPort : htons(ctx->client_Port);
	Addr.sin_addr.s_addr = inet_addr(ctx->client_ip);

	ctx->lastTxPort = Addr.sin_port;

	#ifdef _WIN32
		rc = sendto(ctx->serverSock, (const char*)ctx->txBuf ,(size_t)ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->serverSock, ctx->txBuf, (size_t)ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc < 0)
	{
		printf("sendto returns error: rc=%d\n", rc);
		return 0;
	}

	return 1;
}


//initiates recieve protocol
// pf - pointer to protolcol packet structure
static void init_receive_pkt(prot_frame_info_t *pf)
{
	pf->state = READ_OPT_CODE_1;
	pf->rxLen = 0;
}

// parces recieved packet and fills out prot_frame_info_t structure
// pf - pointer to protolcol packet structure
// pktBuf - pointer to buffer contating the recieved packet
// pktBufLen - length of recieved buffer
// Returns 1=success, 0=failed (malformed packet)
static int receive_tftp_pkt(prot_frame_info_t *pf, uint8_t *pktBuf, int pktBufLen)
{
	int dataLen;
	int n =0;
	int i;
	uint16_t *p;

	if (pktBufLen < 4)
		return 0;

	if (pktBuf[n++] != 0)
		return 0;

	pf->optcode = (uint16_t)pktBuf[n++];

	switch (pf->optcode)
	{
	case TFTP_DATA:
		p = (uint16_t*)&pktBuf[n];

		pf->blocknum = ntohs(*p);
		n += 2;

		dataLen = pktBufLen -4;

		if (dataLen < PROT_MAX_DATA)
			pf->isLastDataBlock = 1;
		else
			pf->isLastDataBlock = 0;

		for (i = 0; i < dataLen; i++)
		{
			pf->dataBuf[i] = pktBuf[n++];
		}
		break;

	case TFTP_ACK:
		p = (uint16_t*)&pktBuf[n];

		pf->blocknum = ntohs(*p);
		break;

	case TFTP_RRQ:
	case TFTP_WRQ:

		i = 0;
		while ((n != 0x00) && (i < PROT_MAX_DATA))
		{
			pf->filename[i++] = pktBuf[n++];
		}

		pf->filename[i++] = 0x00;
		n++;

		while ((n != 0x00) && (i < MAX_MODE_BUFF))
		{
			pf->mode[i++] = pktBuf[n++];
		}

		pf->mode[i++] = 0x00;
		break;

	case TFTP_ERROR:
		p = (uint16_t*)&pktBuf[n];

		pf->errCode= ntohs(*p);
		n += 2;

		i = 0;
		while((n != 0x00) && (i < PROT_MAX_DATA))
		{
			pf->errMessage[i++] = pktBuf[n++];
		}

		pf->errMessage[i++]= 0x00;
		break;

	default:
		return 0;
	}

	pf->rxLen =(uint16_t)pktBufLen;
	return 1;
}

//sends first request to server
//ctx - pointer to client session context
//operationStr - pointer to string buffer containing operation request (getfile or putfile)
//filename - pointer to string buffer containing the filename
static int send_first_request(client_session_t *ctx, const char* operationStr, const char* filename)
{
	size_t n = 0;
	int rc;
	int filenameLen;
	struct sockaddr_in Addr;
	socklen_t AddrSize = (socklen_t)sizeof(Addr);
	const char *mode = "octet";

	//check buffer overflow
	if (strlen(filename) > PROT_MAX_DATA)
		return 0;

	if (strcmp(operationStr, "getfile") == 0)
	{
		ctx->txBuf[n++] = 0x00;
		ctx->txBuf[n++] = TFTP_RRQ;
		ctx->nextExpectedBlockNum = 1;

		client_change_state(ctx, CL_ST_GETFILE_RXDATA);
	}
	else
	{
		ctx->txBuf[n++] = 0x00;
		ctx->txBuf[n++] = TFTP_WRQ;
		ctx->nextExpectedBlockNum = 0;

		client_change_state(ctx, CL_ST_PUTFILE_TXDATA);
	}

	UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS); 	//start waitong for ack tmr

	if (!gFsmDebugOn)
		UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);	//start print progress tmr

	filenameLen = strlen(filename);

	//copy filename into packet buffer
	memcpy(&ctx->txBuf[n], filename, filenameLen+1);
	n += (filenameLen + 1);

	memcpy(&ctx->txBuf[n], mode, strlen(mode)+1);
	n += (strlen(mode) + 1);
	ctx->txLen = n;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(ctx->remotePort);
	Addr.sin_addr.s_addr = inet_addr(ctx->remoteIpStr);

	ctx->lastTxPort = Addr.sin_port;

	#ifdef _WIN32
		rc = sendto(ctx->clientSock, (const char*)ctx->txBuf , n, 0, (struct sockaddr *)&Addr, AddrSize);
	#else
		rc = sendto(ctx->clientSock, ctx->txBuf, n, 0, (struct sockaddr *)&Addr, AddrSize);
	#endif

	if (rc == -1)
	{
		printf("failed to send first request\n");
		return 0;
	}

	return 1;
}


//menu shown to the user if command line input is incorrect
static void help(void)
{
	printf("help:\n");
	printf("-m <operating mode>\n-p <Server Port Number>\n-r <Remote IP Address>\n-o <Operation>\n-f <filename>\n");
}

//safely closes socket
//sock - pointer to socket
static void close_socket(SOCKET *sock)
{
	if (*sock == INVALID_SOCKET)
		return;

	#ifdef _WIN32
		closesocket(*sock);
		WSACleanup();
	#else
		close(*sock);
	#endif

	*sock = INVALID_SOCKET;
}

//safely closes the socket and file
//ctx - pointer to client session context
static void cl_close_file_and_sock(client_session_t*ctx)
{

	close_socket(&ctx->clientSock);

	if (ctx->pFile != NULL)
	{
		fclose(ctx->pFile);
		ctx->pFile = NULL;
	}
}

//safely closes the socket and file
//ctx - pointer to server session context
static void svr_close_file_and_sock(server_session_t*ctx)
{
	close_socket(&ctx->serverSock);

	if (ctx->pFile != NULL)
	{
		fclose(ctx->pFile);
		ctx->pFile = NULL;
	}
}

//send ACK packet from client
//ctx - pointer to client session context
// 1 - success, 0 - failure
static int cl_send_ack(client_session_t *ctx)
{
	int n = 0;
	int rc;
	struct sockaddr_in Addr;

	ctx->txBuf[n++] = 0x00;
	ctx->txBuf[n++] = TFTP_ACK;

	//put block num in network byte order
	ctx->txBuf[n++] = (ctx->blockNum >> 8) & 0xFF;	// High byte
	ctx->txBuf[n++] = ctx->blockNum & 0xFF;			// Low byte

	ctx->txLen = n;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(ctx->svrPort);
	Addr.sin_addr.s_addr = inet_addr(ctx->remoteIpStr);

	ctx->lastTxPort = Addr.sin_port;

	#ifdef _WIN32
		rc = sendto(ctx->clientSock, (const char*)ctx->txBuf , n, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->clientSock, ctx->txBuf, n, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc == -1)
	{
		return 0;
	}

	return 1;
}

//send ACK packet from server
//ctx - pointer to server session context
// 1 - success, 0 - failure
static int svr_send_ack(server_session_t *ctx)
{
	int n = 0;
	int rc;
	struct sockaddr_in Addr;

	ctx->txBuf[n++] = 0x00;
	ctx->txBuf[n++] = TFTP_ACK;

	//put block num in network byte order
	ctx->txBuf[n++] = (ctx->blockNum >> 8) & 0xFF;	// High byte
	ctx->txBuf[n++] = ctx->blockNum & 0xFF;			// Low byte

	ctx->txLen = n;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(ctx->client_Port);
	Addr.sin_addr.s_addr = inet_addr(ctx->client_ip);

	ctx->lastTxPort = Addr.sin_port;

	#ifdef _WIN32
		rc = sendto(ctx->serverSock, (const char*)ctx->txBuf , n, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->serverSock, ctx->txBuf, n, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc == -1)
	{
		return 0;
	}

	return 1;
}

//send error packet from client
//ctx - pointer to client session context
// errCode - error code
// errMsg - pointer to string buffer containing error message
// 1 - success, 0 - failure
static int cl_send_error_pkt(client_session_t *ctx, uint16_t errCode, const char* errMsg)
{
	int n = 0;
	int rc;
	struct sockaddr_in Addr;

	if (strlen(errMsg) > PROT_MAX_DATA)
		return 0;

	ctx->txBuf[n++] = 0x00;
	ctx->txBuf[n++] = TFTP_ERROR;

	ctx->txBuf[n++] = (uint8_t)((errCode>> 8) & 0xff);
	ctx->txBuf[n++] = (uint8_t)(errCode & 0xff);

	//copy error string into packet buffer
	memcpy(&ctx->txBuf[n++], errMsg, strlen(errMsg));

	ctx->txLen = 5 + strlen(errMsg);

	//null terminate buffer
	ctx->txBuf[ctx->txLen++] = 0x00;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(ctx->svrPort);
	Addr.sin_addr.s_addr = inet_addr(ctx->remoteIpStr);

	#ifdef _WIN32
		rc = sendto(ctx->clientSock, (const char*)ctx->txBuf , ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->clientSock, ctx->txBuf, ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc == -1)
	{
		return 0;
	}

	return 1;
}

//send error packet from server
//ctx - pointer to server session context
// errCode - error code
// errMsg - pointer to string buffer containing error message
// 1 - success, 0 - failure
static int svr_send_error_pkt(server_session_t *ctx, uint16_t errCode, const char* errMsg)
{
	int n = 0;
	int rc;
	struct sockaddr_in Addr;

	if (strlen(errMsg) > PROT_MAX_DATA)
		return 0;

	ctx->txBuf[n++] = 0x00;
	ctx->txBuf[n++] = TFTP_ERROR;

	ctx->txBuf[n++] = (uint8_t)((errCode>> 8) & 0xff);
	ctx->txBuf[n++] = (uint8_t)(errCode & 0xff);

	//copy error string into packet buffer
	memcpy(&ctx->txBuf[n++], errMsg, strlen(errMsg));

	ctx->txLen = 5 + strlen(errMsg);

	//null terminate buffer
	ctx->txBuf[ctx->txLen++] = 0x00;

	//send buffer
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(ctx->client_Port);
	Addr.sin_addr.s_addr = inet_addr(ctx->client_ip);

	#ifdef _WIN32
		rc = sendto(ctx->serverSock, (const char*)ctx->txBuf , ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#else
		rc = sendto(ctx->serverSock, ctx->txBuf, ctx->txLen, 0, (struct sockaddr *)&Addr, sizeof(Addr));
	#endif

	if (rc == -1)
	{
		return 0;
	}

	return 1;
}

//sends data to server
//ctx - pointer to client session context
// ev - client event
static int cl_putfile_txData(client_session_t *ctx, int ev)
{
	size_t bytesToRead = PROT_MAX_DATA;
	size_t bytesRead;
	static int bytes_sent = 0;
	int rc;
	int n;

	switch(ev)
	{
	case EV_CL_TIMEOUT:
		//resend ack and increment retransmission tries, and break
		ctx->num_retrans_tries++;

		//close socket if we reach ,ax retransissions and return 0;
		if (ctx->num_retrans_tries == gMaxNumRetransTries)
		{
			ctx->num_retrans_tries = 0;

			printf("reached max number of timeouts, closing session\n");
			cl_send_error_pkt(ctx, 0, "timeout waiting for ack, closing connection\n");
			cl_close_file_and_sock(ctx);
			gDone = 1;
			return 0;
		}

		//resend packet
		cl_send_packet_buffer(ctx, 1);

		UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
		break;

	case EV_CL_PDU_RX:
		switch (ctx->rxInfo.optcode)
		{
		case TFTP_ACK:
			//check block num
			if (ctx->rxInfo.blocknum != ctx->nextExpectedBlockNum)
				break;

			ctx->nextExpectedBlockNum++;

			//send data and restart tmr
			ctx->blockNum++;
			ctx->num_retrans_tries = 0;

			bytesRead = fread(&ctx->txBuf[4], 1, bytesToRead, ctx->pFile);

			// If zero, then this is the end of file
			if ((bytesRead == 0) && (ctx->isFirstDataBlock == 0))
			{
				// Need to deternibe if we need to send an empty DATA block
				if ((ctx->txLen - 4) < PROT_MAX_DATA)
				{
					// If last data block sent was shorter than 512
					// close connection, success
					printf("%s successfully uploaded, closing connection\n", ctx->filename);
					return 0;
				}
			}

			n = 0;
			ctx->txBuf[n++] = 0x00;
			ctx->txBuf[n++] = TFTP_DATA;
			ctx->txBuf[n++] = (uint8_t)((ctx->blockNum >> 8) & 0xff);
			ctx->txBuf[n++] = (uint8_t)(ctx->blockNum & 0xff);

			ctx->txLen = (uint16_t)(4 + bytesRead);

			//send data
			rc = cl_send_packet_buffer(ctx, 0);

			if (!rc)
			{
				printf("error sending data packet, closing connection\n");
				cl_send_error_pkt(ctx,0, "error sending data packet, closing connection");

				cl_close_file_and_sock(ctx);
				return 0;
			}

			bytes_sent += bytesRead;

			if((!gFsmDebugOn) && (UtilTickTimerRun(&ctx->tmr2)))
			{
				printf("bytes sent: %d\n", bytes_sent);
				UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);
			}

			//restart tmr
			UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);

			ctx->isFirstDataBlock = 0;

			return 1;

		case TFTP_ERROR:
			//parse error packet and print to console

			printf("error code: %hu\n", ctx->rxInfo.errCode);

			printf("%s\n", ctx->rxInfo.errMessage);
			cl_close_file_and_sock(ctx);
			return 0;

		default:
			printf("error unexpected optcode recieved, closing connection\n");

			cl_send_error_pkt(ctx, 0, "error unexpected optcode recieved");

			cl_close_file_and_sock(ctx);
			return 0;
		}
		break;
	}
	return 1;
}

//recieves data from server
//ctx - pointer to client session context
// ev - client event
static int cl_getfile_rxData(client_session_t *ctx, int ev)
{
	size_t bytesWritten;
	static int bytes_recieved = 0;

	switch (ev)
	{
		case EV_CL_TIMEOUT:
			//resend ack and incremt retransmission tries, and break
			ctx->num_retrans_tries++;

			//close socket if we reach ,ax retransissions and return 0;
			if (ctx->num_retrans_tries == gMaxNumRetransTries)
			{
				printf("reached max number of timouts, closing session\n");
				ctx->num_retrans_tries = 0;

				cl_send_error_pkt(ctx, 0, "timeout waiting for data, closing connection");
				cl_close_file_and_sock(ctx);
				gDone = 1;
				return 0;
			}

			//send last buffer
			cl_send_packet_buffer(ctx, 1);

			UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
			break;

		case EV_CL_PDU_RX:
			switch (ctx->rxInfo.optcode)
			{
			case TFTP_DATA:
				//compare received block no with expected block no
				//If they mismatch, ignore the packet and break;
				if (ctx->rxInfo.blocknum != ctx->nextExpectedBlockNum)
					break;

				// If they match - proceed
				// Increment next expeted block number
				ctx->nextExpectedBlockNum++;

				if (ctx->isFirstDataBlock)
				{
					//open file for writing
					ctx->pFile = fopen(ctx->filename, "wb");

					if (ctx->pFile == NULL)
					{
						printf("error: failed to open file for writing\n");
						cl_send_error_pkt(ctx, 1, "error, failed to open file for writing");

						return 0;
					}
					ctx->isFirstDataBlock = 0;
				}

				ctx->num_retrans_tries = 0;

				//recieve packet from client and write payload contents into file
				bytesWritten = fwrite(ctx->rxInfo.dataBuf, 1, ctx->rxInfo.rxLen - 4, ctx->pFile);

				if (bytesWritten != (ctx->rxInfo.rxLen - 4))
				{
					//send error packet
					printf("error writing file data, closing connection, bytesWritten = %d (%u)\n", (int)bytesWritten, ctx->rxInfo.rxLen - 4);

					cl_send_error_pkt(ctx, 0, "error writing file data, closing connection");

					//close connection
					cl_close_file_and_sock(ctx);
					return 0;
				}

				//check if this id the last data packet
				if (ctx->rxInfo.isLastDataBlock)
				{
					printf("%s successfully downloaded, closing connection\n", ctx->filename);
					ctx->blockNum++;
					cl_send_ack(ctx);
					cl_close_file_and_sock(ctx);
					return 0;
				}

				bytes_recieved += (int)bytesWritten;

				if ((!gFsmDebugOn) && (UtilTickTimerRun(&ctx->tmr2)))
				{
					printf("bytes recieved: %d\n", bytes_recieved);
					UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);
				}

				//send ack
				ctx->blockNum++;
				cl_send_ack(ctx);

				UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
				return 1;

			case TFTP_ERROR:
				//parse error packet and print to console

				printf("error code: %hu\n", ctx->rxInfo.errCode);

				printf("%s\n", ctx->rxInfo.errMessage);

				cl_close_file_and_sock(ctx);
				return 0;

			default:
				printf("error unexpected optcode recieved, closing connection\n");

				cl_send_error_pkt(ctx, 0, "error unexpected optcode recieved");

				//close socket
				cl_close_file_and_sock(ctx);
				return 0;
			}

			break;
	}

	return 1;
}

//client finite state machine
//ctx - pointer to client session context
// ev - client event
static int cl_fsm_event(client_session_t *ctx, int ev)
{
	if (gFsmDebugOn)
	{
		char stateName[64];
		char eventName[64];

		client_get_state_name(ctx->state, stateName);
		client_get_event_name(ev, eventName);

		printf("CL FSM: ev [%s] <-- %s\n",
			stateName, eventName);
	}

	switch (ctx->state)
	{
	case CL_ST_GETFILE_RXDATA:
		if (!cl_getfile_rxData(ctx, ev))
			return 0;
		break;

	case CL_ST_PUTFILE_TXDATA:
		if (!cl_putfile_txData(ctx, ev))
			return 0;
		break;
	}
	return 1;
}

//waits for first request from client
//ctx - pointer to sevrer session context
// ev - server event
static void svr_wait_first_request(server_session_t *ctx, int ev)
{
	size_t bytesToRead = PROT_MAX_DATA;
	size_t bytesRead;
	int n;

	switch(ev)
	{
	case EV_SVR_PDU_RX:
		switch(ctx->rxInfo.optcode)
		{
		//putfile request, recieving data
		case TFTP_WRQ:
			//get filename, open for writing
			ctx->pFile = fopen((char*)ctx->rxInfo.filename, "wb");

			if (ctx->pFile == NULL)
			{
				printf("error, failed to open file\n");
				svr_send_error_pkt(ctx, 1, "file not found");
				break;
			}

			ctx->filename = (char*)ctx->rxInfo.filename;

			printf("recieved request to write data to file '%s'\n", ctx->filename);

			//send first ack with block num = 0;
			ctx->blockNum = 0;
			ctx->nextExpectedBlockNum = 1;
			svr_send_ack(ctx);

			//start timer
			UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);

			if (!gFsmDebugOn)
				UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);	//start print progress tmr

			server_change_state(ctx, SVR_ST_PUTFILE_RXDATA);
			break;

		//getfile request, sending data
		case TFTP_RRQ:
			//open file for reading
			ctx->pFile = fopen((char*)ctx->rxInfo.filename, "rb");

			if (ctx->pFile == NULL)
			{
				printf("error, failed to open file\n");
				svr_send_error_pkt(ctx, 1, "file not found");
				break;
			}

			ctx->filename = (char*)ctx->rxInfo.filename;

			printf("recieved request to read data from file '%s'\n", ctx->filename);

			//send first data with block num = 1
			ctx->blockNum = 1;
			ctx->nextExpectedBlockNum = 1;

			n = 0;
			ctx->txBuf[n++] = 0x00;
			ctx->txBuf[n++] = TFTP_DATA;
			ctx->txBuf[n++] = (uint8_t)((ctx->blockNum >> 8) & 0xff);
			ctx->txBuf[n++] = (uint8_t)(ctx->blockNum & 0xff);

			//build txBuff
			bytesRead = fread(&ctx->txBuf[n], 1, bytesToRead, ctx->pFile);

			n += bytesRead;

			ctx->txLen = (uint16_t)n;

			//data block, if timeout occurs exit and send error
			if (!svr_send_packet_buffer(ctx, 0))
			{
				printf("error sending data packet\n");

				svr_send_error_pkt(ctx,0, "error sending data packet");

				server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
				break;
			}

			//start timer
			UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);

			if (!gFsmDebugOn)
				UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);	//start print progress tmr

			server_change_state(ctx, SVR_ST_GETFILE_TXDATA);
			break;

		default:

			printf("error sending data packet\n");
			svr_send_error_pkt(ctx,0, "error sending data packet");

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;

		}
		break;

	default:

		server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
		break;

	}
}

//sends data to client
//ctx - pointer to server session context
// ev - server event
static void svr_getfile_txData(server_session_t *ctx, int ev)
{
	size_t bytesToRead = PROT_MAX_DATA;
	size_t bytesRead;
	static int bytes_sent = 0;
	int rc, n;
	switch (ev)
	{
	case EV_SVR_TIMEOUT:
		//resend ack and incremt retransmission tries, and break
		ctx->num_retrans_tries++;

		//close socket if we reach ,ax retransissions and return 0;
		if (ctx->num_retrans_tries == gMaxNumRetransTries)
		{
			ctx->num_retrans_tries = 0;

			printf("reached max number of timouts\n");
			svr_send_error_pkt(ctx, 0, "timeout waiting for ACK, closing connection\n");

			//close file
			if (ctx->pFile != NULL)
			{
				fclose(ctx->pFile);
				ctx->pFile = NULL;
			}

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;
		}

		//resend buffer
		svr_send_packet_buffer(ctx, 1);

		UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
		break;

	case EV_SVR_PDU_RX:
		//check optcodes
		switch (ctx->rxInfo.optcode)
		{
		case TFTP_ACK:
			//send next data block from file
			//check block num
				if (ctx->rxInfo.blocknum != ctx->nextExpectedBlockNum)
					break;

				ctx->nextExpectedBlockNum++;

				//increment block num
				ctx->blockNum++;
				ctx->num_retrans_tries = 0;

				bytesRead = fread(&ctx->txBuf[4], 1, bytesToRead, ctx->pFile);

				// If zero, then this is the end of file
				if (bytesRead == 0)
				{
					// Need to deternibe if we need to send an empty DATA block
					if ((ctx->txLen - 4) < PROT_MAX_DATA)
					{
						// If last data block sent was shorter than 512
						// close connection, success
						printf("%s successfully uploaded\nwaiting for next request\n", ctx->filename);

						//close file
						if (ctx->pFile != NULL)
						{
							fclose(ctx->pFile);
							ctx->pFile = NULL;
						}

						server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
						break;;
					}
				}

				n = 0;
				ctx->txBuf[n++] = 0x00;
				ctx->txBuf[n++] = TFTP_DATA;
				ctx->txBuf[n++] = (uint8_t)((ctx->blockNum >> 8) & 0xff);
				ctx->txBuf[n++] = (uint8_t)(ctx->blockNum & 0xff);

				//printf("DATA HDR: %02x %02x %02x %02x\n", ctx->txBuf[0], ctx->txBuf[1], ctx->txBuf[2], ctx->txBuf[3]);

				ctx->txLen = (uint16_t)(4 + bytesRead);

				// send data block
				rc = svr_send_packet_buffer(ctx, 0);

				if (!rc)
				{
					svr_send_error_pkt(ctx,0, "error sending data packet, closing connection");

					server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
					break;
				}

				bytes_sent += bytesRead;

				if((!gFsmDebugOn) && (UtilTickTimerRun(&ctx->tmr2)))
				{
					printf("bytes sent: %d\n", bytes_sent);
					UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);
				}

				//restart tmr
				UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);

			break;
		case TFTP_ERROR:
			//get error message;
			printf("error code: %hu\n", ctx->rxInfo.errCode);
			printf("%s\n", ctx->rxInfo.errMessage);

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;

		default:
			printf("error, unexpected optcode\n");
			svr_send_error_pkt(ctx, 0, "error, unexpected optcode");

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;
		}
		break;
	}
}

//recieves data from client
//ctx - pointer to server session context
// ev - server event
static void svr_putfile_rxData(server_session_t *ctx, int ev)
{
	size_t bytesWritten;
	static int bytes_recieved = 0;
	switch (ev)
	{
	case EV_SVR_TIMEOUT:
		//resend ack and incremt retransmission tries, and break
		ctx->num_retrans_tries++;

		//close socket if we reach ,ax retransissions and return 0;
		if (ctx->num_retrans_tries == gMaxNumRetransTries)
		{
			printf("reached ,max number of timouts\n");
			svr_send_error_pkt(ctx, 0, "timeout waiting for ACK, closing connection\n");

			ctx->num_retrans_tries = 0;

			//close file
			if (ctx->pFile != NULL)
			{
				fclose(ctx->pFile);
				ctx->pFile = NULL;
			}

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;
		}

		//resend buffer
		svr_send_packet_buffer(ctx, 1);

		UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
		break;

	case EV_SVR_PDU_RX:

		switch (ctx->rxInfo.optcode)
		{
		case TFTP_DATA:
			//compare received block no with expected block no
			//If they mismatch, ignore the packet and break;
			if (ctx->rxInfo.blocknum != ctx->nextExpectedBlockNum)
				break;

			// If they match - proceed
			// Increment next expeted block number
			ctx->nextExpectedBlockNum++;
			ctx->num_retrans_tries = 0;

			//recieve packet from client and write payload contents into file
			bytesWritten = fwrite(ctx->rxInfo.dataBuf, 1, ctx->rxInfo.rxLen - 4, ctx->pFile);

			if (bytesWritten != (ctx->rxInfo.rxLen - 4))
			{
				//send error packet
				printf("error writing file data, closing connection, bytesWritten = %d (%u)\n", (int)bytesWritten, ctx->rxInfo.rxLen - 4);

				svr_send_error_pkt(ctx, 0, "error writing file data, closing connection");

				server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
				break;
			}

			//check if this id the last data packet
			if (ctx->rxInfo.isLastDataBlock)
			{
				printf("%s has been successfully downloaded\nwaiting for next request\n", ctx->filename);
				ctx->blockNum++;
				svr_send_ack(ctx);

				if (ctx->pFile != NULL)
				{
					fclose(ctx->pFile);
					ctx->pFile = NULL;
				}

				server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
				break;
			}

			bytes_recieved += (int)bytesWritten;

			if ((!gFsmDebugOn) && (UtilTickTimerRun(&ctx->tmr2)))
			{
				printf("bytes recieved: %d\n", bytes_recieved);
				UtilTickTimerStart(&ctx->tmr2, PROGRESS_TMR_SEC);
			}

			//send ack
			ctx->blockNum++;
			svr_send_ack(ctx);

			UtilTickTimerStart(&ctx->tmr1, ACK_TIMEOUT_SECS);
			break;
		case TFTP_ERROR:
			//get error message;
			printf("error code: %hu\n", ctx->rxInfo.errCode);
			printf("%s\n", ctx->rxInfo.errMessage);

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;

		default:
			printf("error, unexpected optcode\n");
			svr_send_error_pkt(ctx, 0, "error, unexpected optcode");

			server_change_state(ctx, SVR_ST_WAIT_FIST_REQUEST);
			break;
		}
		break;
	}
}

// server finite state machine
//ctx - pointer to server session context
// ev - server event
static void svr_fsm_event(server_session_t *ctx, int ev)
{
	if (gFsmDebugOn)
	{
		char stateName[64];
		char eventName[64];

		server_get_state_name(ctx->state, stateName);
		server_get_event_name(ev, eventName);

		printf("SVR FSM: ev [%s] <-- %s\n",stateName, eventName);
	}

	switch (ctx->state)
	{
	case SVR_ST_GETFILE_TXDATA:
		svr_getfile_txData(ctx, ev);
		break;

	case SVR_ST_PUTFILE_RXDATA:
		svr_putfile_rxData(ctx, ev);
		break;

	case SVR_ST_WAIT_FIST_REQUEST:
		svr_wait_first_request(ctx, ev);
		break;
	}
}

//creates client socket
//client_sock - created socket
static int create_outgoing_con_sock(SOCKET *client_sock)
{
	struct sockaddr_in Addr;
	SOCKET sock;

	#ifdef _WIN32
		WSADATA wsaData;

		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			printf("Failed to initialize Winsock\n");
			return 0;
		}
	#endif

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock == INVALID_SOCKET)
	{
		printf("failed to create outgoing connection socket\n");
		return 0;
	}

	//bind socket
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_addr.s_addr = INADDR_ANY;
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(0);

	if (bind(sock, (struct sockaddr *)&Addr, sizeof(struct sockaddr_in)) == -1)
	{
		printf("failed to bind socket to local udp port\n");
		close_socket(&sock);
		return 0;
	}

	*client_sock = sock;
	return 1;
}

//runs the client application
// remote_ip - remote IP address
//filename - pointer to string buffer containing filename
//operation - operate request (getfile or putfile)
// returns 0 - error occured, 1 - session ended normally
static int file_client(const char *remote_ip, const char *filename, const char* operation)
{
	fd_set readfds;
	int ret, rc;
	uint8_t rxbuf[MAX_RX_BUFF];
	struct timeval selTimeout;
	int timeout_ms = 15;
	int packetCount = 0;

	tick_timer_t connectionTmr;	//waitng for connection timer
	uint32_t ConTimeout = 5;	//seconds

	struct sockaddr_in from;

	memset(&clientCtx, 0, sizeof(clientCtx));

	clientCtx.filename = filename;
	clientCtx.remoteIpStr = remote_ip;
	clientCtx.remotePort = gSrvPort;

	clientCtx.isFirstDataBlock = 1;

	if (!create_outgoing_con_sock(&clientCtx.clientSock))
		return 0;

	if (strcmp(operation, "putfile") == 0)
	{
		printf("starting TFTP file upload: remote IP %s, port %hu\n", remote_ip, gSrvPort);
	}
	else
	{
		printf("starting TFTP file download: remote IP %s, port %hu\n", remote_ip, gSrvPort);
	}

	// select will wait on events for x milliseconds
	selTimeout.tv_sec = (timeout_ms / 1000);
	selTimeout.tv_usec = ((timeout_ms % 1000) * 1000);

	if (strcmp(operation, "putfile") == 0)
		clientCtx.pFile = fopen(clientCtx.filename, "rb");

	if ((strcmp(operation, "putfile") == 0) && (clientCtx.pFile == NULL))
	{
		printf("error: failed to open file for reading\n");
		cl_send_error_pkt(&clientCtx, 1, "error, failed to open file for reading");
		return 0;
	}

	send_first_request(&clientCtx, operation, filename);

	UtilTickTimerStart(&connectionTmr, ConTimeout);

	while (!gDone)
	{
		// potentially perform other tasks

		// Setup socket sock to be monitored for "read events"
		FD_ZERO(&readfds);
		FD_SET(clientCtx.clientSock, &readfds);

		ret = select(clientCtx.clientSock + 1, &readfds, NULL, NULL, &selTimeout);

		if (ret > 0)
		{
			if (FD_ISSET(clientCtx.clientSock, &readfds))
			{
				// Data is available for reading from the socket
				// call socket receive function here
				#ifdef _WIN32
				int addrlen = sizeof(from);
					rc = recvfrom(clientCtx.clientSock, (char *)rxbuf, sizeof(rxbuf), 0, ((struct sockaddr *)&from), &addrlen);
				#else
				socklen_t addrlen = sizeof(from);
					rc = recvfrom(clientCtx.clientSock, rxbuf, sizeof(rxbuf), 0, ((struct sockaddr *)&from), &addrlen);
				#endif

				if (rc > 0)
				{
					packetCount ++;

					clientCtx.svrPort = ntohs(from.sin_port);

					init_receive_pkt(&clientCtx.rxInfo);

					//call recieve packet function for all packets that are a multiple of 5
					if (gDebugDropPacket && ((packetCount % 5) == 0))
					{
						printf("%dth packet dropped\n", packetCount);
					}
					else if (gDebugDropAllPks && (packetCount < 10))
					{
						// data received in rxbuf, length od data returned in rc
						if (receive_tftp_pkt(&clientCtx.rxInfo, rxbuf, rc))
						{
							if (!cl_fsm_event(&clientCtx, EV_CL_PDU_RX))
								break;
						}
						else
						{
							printf("receive_tftp_pkt returned 0\n");
						}
					}
					else if (!gDebugDropAllPks)
					{
						// data received in rxbuf, length od data returned in rc
						if (receive_tftp_pkt(&clientCtx.rxInfo, rxbuf, rc))
						{
							if (!cl_fsm_event(&clientCtx, EV_CL_PDU_RX))
								break;
						}
						else
						{
							printf("receive_tftp_pkt returned 0\n");
						}
					}
				}
				else if (UtilTickTimerRun(&connectionTmr))
				{
					printf("no response from server, closing session\n");
					gDone = 1;
				}
			}
		}

		if (UtilTickTimerRun(&clientCtx.tmr1))
			cl_fsm_event(&clientCtx, EV_CL_TIMEOUT);
	}

	cl_close_file_and_sock(&clientCtx);
	return 1;
}

//creates server socket
//server_sock - created socket
static int create_svr_sock(SOCKET *server_sock)
{
	struct sockaddr_in Addr;
	SOCKET sock;

	#ifdef _WIN32
		WSADATA wsaData;

		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			printf("Failed to initialize Winsock\n");
			return 0;
		}
	#endif

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock == INVALID_SOCKET)
	{
		printf("failed to create outgoing connection socket\n");
		return 0;
	}

	#ifndef _WIN32
		// Set SO_REUSEADDR option
		int opt = 1;

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
		{
			perror("setsockopt failed");
			close_socket(&sock);
			return 0;
		}
	#endif

	//bind socket
	memset(&Addr, 0, sizeof(struct sockaddr_in));
	Addr.sin_addr.s_addr = INADDR_ANY;
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(gSrvPort);

	if (bind(sock, (struct sockaddr *)&Addr, sizeof(struct sockaddr_in)) == -1)
	{
		printf("failed to bind socket to local udp port (%s)\n", strerror(errno));
		close_socket(&sock);
		return 0;
	}

	*server_sock = sock;
	return 1;
}

//runs the server application
// returns 0 - error occurred
//returns 1 - user ended session
static int file_server()
{
	fd_set readfds;
	int ret, rc;
	uint8_t rxbuf[MAX_RX_BUFF];
	struct timeval selTimeout;
	int timeout_ms = 15;

	struct sockaddr_in from;

	memset(&serverCtx, 0, sizeof(serverCtx));

	if(!create_svr_sock(&serverCtx.serverSock))
		return 0;

	// select will wait on events for x milliseconds
	selTimeout.tv_sec = (timeout_ms / 1000);
	selTimeout.tv_usec = ((timeout_ms % 1000) * 1000);

	server_change_state(&serverCtx, SVR_ST_WAIT_FIST_REQUEST);

	printf("server up, waiting for client requests\n");

	while(!gDone)
	{
		// Setup socket sock to be monitored for "read events"
		FD_ZERO(&readfds);
		FD_SET(serverCtx.serverSock, &readfds);

		ret = select(serverCtx.serverSock + 1, &readfds, NULL, NULL, &selTimeout);

		if (ret > 0)
		{
			if (FD_ISSET(serverCtx.serverSock, &readfds))
			{
				// Data is available for reading from the socket
				// call socket receive function here
				#ifdef _WIN32
				int addrlen = sizeof(from);
					rc = recvfrom(serverCtx.serverSock, (char *)rxbuf, sizeof(rxbuf), 0, ((struct sockaddr *)&from), &addrlen);
				#else
				socklen_t addrlen = sizeof(from);
					rc = recvfrom(serverCtx.serverSock, rxbuf, sizeof(rxbuf), 0, ((struct sockaddr *)&from), &addrlen);
				#endif

				//retieve cleint port number and covert to host byte order
				serverCtx.client_Port= ntohs(from.sin_port);
				serverCtx.client_ip = inet_ntoa(from.sin_addr);

				init_receive_pkt(&serverCtx.rxInfo);

				if (rc > 0)
				{
					if (receive_tftp_pkt(&serverCtx.rxInfo, rxbuf, rc))
					{
						svr_fsm_event(&serverCtx, EV_SVR_PDU_RX);
					}
					else
					{
						printf("receive_tftp_pkt() returned 0\n");
					}
				}
			}
		}

		if (UtilTickTimerRun(&serverCtx.tmr1))
			svr_fsm_event(&serverCtx, EV_SVR_TIMEOUT);

	}
	svr_close_file_and_sock(&serverCtx);
	return 1;
}

// <operating mode> <Server Port Number> <Remote IP Address><Operation> <Filename> <Fsm_debug_on> <DebugDropTxAckOn><max_retransmission_tries> <drop all packes>
// example: TFTP.exe -m server -p 1234 -r 198.678.0.8 -o putfile -f filename.txt
int main(int argc, char *argv[])
{
	int c, isClient;

	const char * op_mode = NULL;
	const char* remote_ip_str = NULL;
	const char * operation_str = NULL;
	const char* filename = NULL;
	int Fsm_debug_on = 0;
	int DebugDropTxPacket = 0;

	static const char *kOptString = "m:p:r:o:f:d:D:M:A:";

	static const struct option kLongOpts[] =
	{
		{ "operating mode",  required_argument, NULL, 'm' },
		{ "Server Port Number",  required_argument, NULL, 'p'},
		{ "Remote IP Address",  required_argument, NULL, 'r'},
		{ "Operation",  required_argument, NULL, 'o'},
		{ "Filename",  required_argument, NULL, 'f'},
		{ "Fsm_debug_on",  required_argument, NULL, 'd'},
		{ "DebugDropTxPacket",  required_argument, NULL, 'D'},
		{"max_retransmission_tries", required_argument, NULL, 'M'},
		{"drop all packets", required_argument, NULL, 'A'},
		{ NULL, 0, NULL, 0 }
	};

	if (argc < 1)
	{
		help();
		return 0;
	}

	while ((c = getopt_long(argc, argv, kOptString, kLongOpts, NULL)) != -1)
	{
		switch (c)
		{
			case 'm': op_mode = optarg;break;
			case 'p': gSrvPort = (uint16_t)atoi(optarg); break;
			case 'r': remote_ip_str = optarg; break;
			case 'o' : operation_str = optarg; break;
			case 'f' : filename = optarg; break;
			case'd' : Fsm_debug_on = atoi(optarg); break;
			case 'D' : DebugDropTxPacket = atoi(optarg); break;
			case 'M' : gMaxNumRetransTries = atoi(optarg); break;
			case 'A' : gDebugDropAllPks= atoi(optarg); break;

			default : help(); return 0;
		}
	}

	isClient = 0;

	if (gSrvPort == 0)
	{
		printf("error, invalid port number\n");
		return 0;
	}

	if ((strcmp(op_mode, "client") != 0) && (strcmp(op_mode, "server") != 0))
	{
		printf("error: invalid operating mode\n");
		return 0;
	}

	isClient = (strcmp(op_mode, "client") == 0) ? 1 : 0;

	if ((isClient) && ((strcmp(operation_str, "getfile") != 0) && (strcmp(operation_str, "putfile") != 0)))
	{
		printf("error: invalid operation request\n");
		return 0;
	}

	if ((isClient == 1) && (strcmp(remote_ip_str, "0.0.0.0") == 0))
	{
		printf("error: invalid IP address\n");
		return 0;
	}

	#ifdef _WIN32
		SetConsoleCtrlHandler(signal_handler, TRUE);
	#else
		signal(SIGHUP, signal_handler);
		signal(SIGTERM, signal_handler);
		signal(SIGINT, signal_handler);
		signal(SIGQUIT, signal_handler);
	#endif

	if (Fsm_debug_on > 0)
		gFsmDebugOn = 1;

	if (DebugDropTxPacket > 0)
		gDebugDropPacket= 1;

	if (isClient == 0)
	{
		file_server();
	}
	else
	{
		file_client(remote_ip_str,filename, operation_str);
	}

	return 0;
}

