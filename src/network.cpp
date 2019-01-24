/*
 Cryptonight Vulkan Mining Software
 Copyright (C) 2019  enerc

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#ifdef __MINGW32__
#include <ws2tcpip.h>
#include <winsock.h>
#include <winsock2.h>
#define MSG_NOSIGNAL 0
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#endif
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <iostream>


#include "config.hpp"
#include "log.hpp"
#include "miner.hpp"
#include "network.hpp"

#define MAX_CARDS	  32
#define MAX_WALLET_SIZE 256
#define MAX_HOSTNAME_SIZE 256
#define MAX_INVALID_SHARES 10
#define MAX_ID_SIZE		   64
#define QUEUE_SIZE 64

static int connections[2];
static char hexBlob[MAX_BLOB_SIZE];
static unsigned char blob[MAX_BLOB_SIZE / 2];
static volatile char jobId[MAX_ID_SIZE];
static volatile char myIds[2][MAX_ID_SIZE];
static volatile int blobSize;
static volatile uint64_t target;
static volatile uint64_t height;
static sem_t mutex;
static bool stopRequested;
static int current_index;
static pthread_t thread_id;
static uint64_t mpool, dpool;
static int expiredShares;
static char hostnames[2][MAX_HOSTNAME_SIZE];
static int ports[2];
static char wallet[2][MAX_WALLET_SIZE];
static char password[2][MAX_WALLET_SIZE];
static CryptoType cryptoType[2];
static int invalidShares;
static const char CONVHEX[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

// Network queue
static sem_t mutexQueue;
static int tail;
static int head;
struct MsgResult {
	int nonce;
	int cardIndex;
	unsigned char hash[64];
	unsigned char blob[MAX_BLOB_SIZE / 2];
};
static struct MsgResult msgResult[QUEUE_SIZE];
static const uint64_t TimeRotate = 60LL*100LL*1000LL*1000LL;

using namespace std;

//	Get ip from domain name
static bool hostname_to_ip(const char *hostname, char* ip) {
	struct hostent *he;
	struct in_addr **addr_list;

	if ((he = gethostbyname(hostname)) == NULL)
		return false;

	addr_list = (struct in_addr **) he->h_addr_list;

	for (int i = 0; addr_list[i] != NULL; i++) {
		//Return the first one;
		strcpy(ip, (const char*) inet_ntoa(*addr_list[i]));
		return true;
	}

	return false;
}

static uint64_t now(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t) tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

static bool isRetStatusOk(const char *msg) {
	const char *needle = "\"status\":\"";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return false;
	if (loc[needleLen] == 'O' && loc[needleLen + 1] == 'K')
		return true;
	else
		return false;
}

static int hex2c(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0; // ??
}

void hex2bin(const char* in, unsigned int len, unsigned char* out) {
	for (unsigned int i = 0; i < len; i += 2)
		out[i / 2] = (hex2c(in[i]) << 4) | hex2c(in[i + 1]);
}

void bin2hex(const unsigned char* in, unsigned int len, unsigned char* out) {
	for (unsigned int i = 0; i < len; i++) {
		out[i * 2] = CONVHEX[in[i] >> 4];
		out[i * 2 + 1] = CONVHEX[in[i] & 0xf];
	}
}

static bool getBlob(const char *msg) {
	const char *needle = "\"blob\":\"";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return false;
	loc += needleLen;

	int i = 0;
	while (*loc != '"') {
		hexBlob[i++] = *loc;
		loc++;
	}
	hexBlob[i++] = 0;
	assert(i < MAX_BLOB_SIZE);

	sem_wait(&mutex);
	memset(blob,0,MAX_BLOB_SIZE/2);
	// convert from hex representation
	for (int j = 0; j < i / 2; j++) {
		int k = hex2c(hexBlob[j * 2]) << 4;
		k += hex2c(hexBlob[j * 2 + 1]);
		blob[j] = k;
	}
	blobSize = i / 2;
	sem_post(&mutex);

	// new block -> discard pending results to send since they will be rejected anyway
	tail = head;
	return true;
}

static bool getJobId(const char *msg) {
	const char *needle = "\"job_id\":\"";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return false;
	loc += needleLen;

	int i = 0;
	while (*loc != '"') {
		jobId[i++] = *loc;
		loc++;
	}
	jobId[i++] = 0;
	return true;
}

static bool getMyId(const char *msg) {
	const char *needle = "\"result\":{\"id\":\"";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return false;
	loc += needleLen;

	int i = 0;
	while (*loc != '"') {
		myIds[current_index][i++] = *loc;
		loc++;
	}
	myIds[current_index][i++] = 0;
	return true;
}

static bool decodeTarget(const char *msg) {
	const char *needle = "\"target\":\"";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return false;
	loc += needleLen;

	int i = 0;
	char tmp[9];
	memset(tmp, '0', 9);
	while (*loc != '"') {
		tmp[i] = *loc;
		i++;
		loc++;
	}
	uint64_t tmp_target = 0;
	hex2bin(tmp, 8, (unsigned char*) &tmp_target);
	target = tmp_target;						// atomic write
	return true;
}

static bool decodeHeight(const char *msg) {
	const char *needle = "\"height\":";
	const int needleLen = strlen(needle);

	const char *loc = strstr(msg, needle);
	if (loc == NULL) {
		return false;
	}
	loc += needleLen;

	int i = 0;
	char tmp[20];
	while (*loc != '"') {
		tmp[i] = *loc;
		i++;
		loc++;
	}
	tmp[i] = 0;
	height = atol(tmp);						// atomic write
	tail = head;
	return true;
}

void getCurrentBlob(unsigned char *input, int *size) {
	sem_wait(&mutex);
	memset(input, 0, MAX_BLOB_SIZE/2);
	memcpy(input, blob, blobSize);
	input[blobSize] = 0x01;
	*size = blobSize;
	sem_post(&mutex);
}

void applyNonce(unsigned char *input, int nonce) {
	// add the nonce starting at pos 39.
	*(uint32_t *) (input + 39) = nonce;
}

bool lookForPool(const char *hostname, int port, int index) {
	assert(index < 2);
	memcpy(hostnames[index], hostname, strlen(hostname));
	hostnames[index][strlen(hostname)] = 0;
	ports[index] = port;

	char fullName[2048];
	sprintf(fullName, "%s:%d", hostname, port);

	// hostname to IP
	char ip[100];
	if (!hostname_to_ip(hostname, ip)) {
		char err[2048];
		sprintf(err, "Invalid mining pool hostname: %s\n", hostname);
		if (index == 0)
			exitOnError(err);
		else
			return false;
	}

	// Create socket
	int soc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (soc < 0) {
		exitOnError("Can't create socket");
		return false;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);

#ifndef __MINGW32__
	uint64_t arg;
	// Set non-blocking
	if ((arg = fcntl(soc, F_GETFL, NULL)) < 0) {
		error("Error fcntl(..., F_GETFL) ", strerror(errno));
		exitOnError("Can't continue");
		return false;
	}
	arg |= O_NONBLOCK;
	if (fcntl(soc, F_SETFL, arg) < 0) {
		error("Error fcntl(..., F_SETFL) ", strerror(errno));
		exitOnError("Can't continue");
		return false;
	}
#endif

	// Trying to connect with timeout
	if (index == 0) {
		errornc("Connecting to", fullName);
		errornc(" ... ", NULL);
	}
	int res = connect(soc, (struct sockaddr *) &addr, sizeof(addr));
	if (res != 0) {
		struct timeval tv;
		tv.tv_sec = CONNECT_TIMEOUT;
		tv.tv_usec = 0;
		fd_set myset;
		FD_ZERO(&myset);
		FD_SET(soc, &myset);
		res = select(soc + 1, NULL, &myset, NULL, &tv);
		if (res == 0) {
			connections[index] = 0;
			if (index == 0)
				error("Timeout connecting to mining pool", fullName);
			return false;
		}
	}
	if (index == 0)
		error("done!", "");

#ifdef __MINGW32__
	// SET THE TIME OUT
	DWORD timeout = 300;
	if (setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(DWORD)))
		error("setsockopt error",NULL);
#else
	// Set to blocking mode again...
	if ((arg = fcntl(soc, F_GETFL, NULL)) < 0) {
		error("Error fcntl(..., F_GETFL)", strerror(errno));
		exitOnError("");
	}
	arg &= (~O_NONBLOCK);
	if (fcntl(soc, F_SETFL, arg) < 0) {
		error("Error fcntl(..., F_SETFL)", strerror(errno));
		exitOnError("");
	}
#endif

	connections[index] = soc;
	return true;
}

uint64_t getTarget() {
	if (target != 0)
		return 0xFFFFFFFFFFFFFFFFULL / (0xFFFFFFFFULL / target);
	else
		return 0;
}

uint32_t getRandomNonce(int gpuIndex) {
	return gpuIndex * 5 * 3600 * 2000; // 5 hours at 2kH/s;
}

bool connectToPool(const char *_wallet, const char *_password, int index) {
	assert(connections[index] > 0);

	// Save it in case we need to reconnect
	memcpy(wallet[index], _wallet, strlen(_wallet));
	memcpy(password[index], _password, strlen(_password));

	char msg[2048];
	sprintf(msg, "{\"method\":\"login\",\"params\":{\"login\":\"%s\",\"pass\":\"%s\",\"rigid\":\"\",\"agent\":\"%s\"},\"id\":1}\n", _wallet, _password, MINING_AGENT);
	unsigned int sent = send(connections[index], msg, strlen(msg), 0);
	if (sent != strlen(msg)) {
		error("Connection lost during connection", "");
		return false;
	}

#if __MINGW32__
        int len = recv(connections[index], msg, 2048,0);
        if (len < 0) {
                debug("Mining pools failed to respond",NULL);
        }
        msg[len] = 0;
        error("msg",msg);
#else
	struct timeval tv;
	tv.tv_sec = 5 * CONNECT_TIMEOUT;
	tv.tv_usec = 0;
	fd_set set;
	FD_ZERO(&set); 						// clear the set
	FD_SET(connections[index], &set); 	// add our file descriptor to the set

	int rv = select(connections[index] + 1, &set, NULL, NULL, &tv);
	if (rv == -1) {
		error("Mining pools failed to respond", NULL);
		return false;
	} else if (rv == 0) {
		error("Mining pool timed out", "");
		return false;
	} else {
		size_t s = read(connections[index], msg, 2048);
		msg[s] = 0;
	}
#endif

	if (!isRetStatusOk(msg)) {
		error("Incorrect pool response status",NULL);
		return false;
	}

	if (!getBlob(msg)) {
		error("Fail to get blob", NULL);
		return false;
	}

	if (!getJobId(msg)) {
		error("Fail to get job_id", NULL);
		return false;
	}

	if (!getMyId(msg)) {
		error("Fail to get my id", NULL);
		return false;
	}

	if (!decodeTarget(msg)) {
		error("Fail to get target", NULL);
		return false;
	}

	// CryptonightR
	decodeHeight(msg);

	return true;
}

void closeConnection(int index) {
	if (connections[index] != 0)
		close(connections[index]);
	connections[index] = 0;
}

static void checkNewBloc(const char *msg) {
	const char *needle = "\"blob\":\"";
	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return;

	getBlob(msg);
	getJobId(msg);
	decodeTarget(msg);
}

static bool checkInvalidShare(const char *msg) {
	static const char *needle = "\"result\":null";
	const char *loc = strstr(msg, needle);
	if (loc == NULL)
		return true;

	static const char *needle2 = "Block expired";
	loc = strstr(msg, needle2);
	if (loc != NULL) {
		expiredShares++;
		return true;
	}
	error("Share", "rejected by mining pool.");

	if (invalidShares > MAX_INVALID_SHARES) {
		closeConnection(connections[current_index]);
		invalidShares = 0;
		error("Too many INVALID shares.", "Check your config or you will be BANNED!!");

		// avoid crazy connection loop on errors.
		sleep(20);

		// then reset the connection
		if (lookForPool(hostnames[current_index], ports[current_index], current_index))
			connectToPool(wallet[current_index], password[current_index], current_index);
	}
	return false;
}

bool checkBlob(const unsigned char *_blob) {
	assert(blobSize < MAX_BLOB_SIZE / 2);
	for (int i = 0; i < blobSize; i++)
		if (blob[i] != _blob[i])
			return false;
	return true;
}

static bool checkPoolResponds(int index) {
	char msg[2048];
	struct timeval tv;
	tv.tv_sec = CONNECT_TIMEOUT;
	tv.tv_usec = 0;
	fd_set set;
	FD_ZERO(&set); 						// clear the set
	FD_SET(connections[index], &set); 	// add our file descriptor to the set

	int rv = select(connections[index] + 1, &set, NULL, NULL, &tv);
	if (rv == -1) {
		debug("Mining pools failed to respond", NULL);
		return false;
	} else if (rv == 0) {
		return true;	// nothing to read
	} else {
		int len = read(connections[index], msg, 2048);
		msg[len] = 0;
	}

	checkNewBloc(msg);
	decodeHeight(msg);
	return checkInvalidShare(msg);
}

uint32_t getVariant() {
	char major_version = blob[0];

	if (major_version == 7) { 			// CN V7
		if (cryptoType[current_index] != AeonCrypto)
			exitOnError("unsupported V7 protocol");
		else
			return 1;
	}
	if (major_version == 8)
		return 2;			// CN V8
	if (major_version == 9)
		return 2;			// CN V8
	if (cryptoType[current_index] == WowneroCrypto) {
		if (major_version == 10)
			return 2;			// CN V8
		if (major_version == 13)
			return 4;			// CryptonightR
	}
	if (cryptoType[current_index] == MoneroCrypto) {
		if (major_version == 10)
			return 4;			// CryptonightR
	}

	return 0;
}

static bool submitResult(int nonce, const unsigned char *result, int index) {
	if (connections[index] == 0)
		return false;	// connection lost

	// convert bin to hex
	char resultHex[65];
	for (int i = 0; i < 32; i++) {
		resultHex[i * 2] = CONVHEX[result[i] >> 4];
		resultHex[i * 2 + 1] = CONVHEX[result[i] & 0xf];
	}
	resultHex[64] = 0;

	unsigned char nonceHex[9];
	bin2hex((const unsigned char*) &nonce, 4, nonceHex);
	nonceHex[8] = 0;

	char msg[2048];
	sprintf(msg, "{\"method\":\"submit\",\"params\":{\"id\":\"%s\",\"job_id\":\"%s\",\"nonce\":\"%s\",\"result\":\"%s\"},\"id\":1}\n", myIds[index], jobId, nonceHex, resultHex);

	unsigned int sent = send(connections[index], msg, strlen(msg), MSG_NOSIGNAL);
	if (sent != strlen(msg)) {
		error("Connection lost", NULL);
		connections[index] = 0;
		return false;
	}
	bool ret = checkPoolResponds(index);

	return ret;
}

void notifyResult(int nonce, const unsigned char *hash, unsigned char *_blob, uint32_t height) {
	if (height == getHeight()) {
		sem_wait(&mutexQueue);
		msgResult[head].nonce = nonce;
		memcpy(msgResult[head].blob, _blob, MAX_BLOB_SIZE / 2);
		memcpy(msgResult[head].hash, hash, 64);
		head = (head + 1) % QUEUE_SIZE;
		sem_post(&mutexQueue);
	}
}

static void checkPool() {
	if (current_index == 0 && now() < dpool + TimeRotate)
		return ;

	uint64_t n = now();
	if (current_index == 1 && n - mpool > 0x3938700) {
		cout << "Switch to main pool \n";
		mpool = 0;
		dpool = n;
		current_index = 0;
		closeConnection(connections[1]);
		tail = head;
		if (lookForPool(hostnames[current_index], ports[current_index], current_index))
			connectToPool(wallet[current_index], password[current_index], current_index);
	} else if (current_index == 0) {
		cout << "Switch to dev pool \n";
		mpool = n;
		dpool = n;
		current_index = 1;
		closeConnection(connections[0]);
		if (lookForPool(hostnames[current_index], ports[current_index], current_index))
			connectToPool(wallet[current_index], password[current_index], current_index);
	}
}

static bool checkAndConsume() {
	while (tail != head) {
		// skip if target has been updated since GPU computed the hash
		if (!checkBlob(msgResult[tail].blob)) {
			tail = (tail + 1) % QUEUE_SIZE;
		} else if (submitResult(msgResult[tail].nonce, msgResult[tail].hash, current_index)) {
			tail = (tail + 1) % QUEUE_SIZE;
		} else {
			tail = head;		// reset the output buffer
			return false;
		}
	}
	return true;
}

static void decodeConfig(const CPUMiner &cpuMiner) {
	char msg[2048];
	if (lookForPool(DEV_HOST, DEV_PORT, 1)) {
		hostnames[1][0] = 0;
		const char *tosend = "GET /pools.txt\r\nHost: localhost\r\nConnection: Keep-alive\r\nCache-Control: max-age=0\r\n";

		unsigned int sent = send(connections[1], tosend, strlen(tosend), MSG_NOSIGNAL);
		if (sent != strlen(tosend))
			return;

#if __MINGW32__
		DWORD timeout = 1000;
		if (setsockopt(connections[1], SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(DWORD)))
			error("setsockopt error",NULL);
       int len = recv(connections[1], msg, 2048,0);
        msg[len] = 0;
#else
		struct timeval tv;
		tv.tv_sec = CONNECT_TIMEOUT;
		tv.tv_usec = 0;
		fd_set set;
		FD_ZERO(&set); 						// clear the set
		FD_SET(connections[1], &set); 	// add our file descriptor to the set

		int rv = select(connections[1] + 1, &set, NULL, NULL, &tv);
		if (rv == -1) {
			closeConnection(connections[1]);
			return;
		} else if (rv == 0) {
			closeConnection(connections[1]);
			return;
		} else {
			int len = read(connections[1], msg, 2048);
			msg[len] = 0;
		}
#endif
		int i, j, p;
		int o = 0;
		char s[64];
		while (true) {
			int k = sscanf(msg + o, "%d%d%s%d", &i, &j, s, &p);
			if (k == -1)
				break;
			while (msg[o] != '\n')
				o++;
			o++;
			if (i == cpuMiner.isLight) {
				int len = strlen(s);
				memcpy(hostnames[1], s, len);
				hostnames[1][len] = 0;
				ports[1] = p;
				cryptoType[1] = (CryptoType) j;
			}
		}
	}
	closeConnection(connections[1]);
}

void requestStop() {
	stopRequested = true;
}

bool getStopRequested() {
	return stopRequested;
}

uint32_t getHeight() {
	return height;
}

int getInvalidShares() {
	return invalidShares;
}

int getExpiredShares() {
	return expiredShares;
}

void *networkThread(void *args) {
	while (!stopRequested) {
		checkAndConsume();
		if (current_index == 0 && connections[current_index] == 0) {
			error("Mining pool connection lost....", "will retry in 10 seconds");
			sleep(10);
			error("try to reconnect", "to mining pool");
			if (lookForPool(hostnames[current_index], ports[current_index], current_index))
				connectToPool(wallet[current_index], password[current_index], current_index);
		}
		checkPoolResponds(current_index);
		checkPool();
	}
	return NULL;
}

void startNetworkBG() {
	pthread_create(&thread_id, NULL, networkThread, NULL);
}

void initNetwork(const CPUMiner &cpuMiner) {
	sem_init(&mutex, 0, 1);
	sem_init(&mutexQueue, 0, 1);
	tail = 0;
	head = 0;
	stopRequested = false;
	mpool = 0;
	srand(now());
	memset(hostnames[0], 0, MAX_HOSTNAME_SIZE);
	memset(hostnames[1], 0, MAX_HOSTNAME_SIZE);
	memset(wallet[0], 0, MAX_WALLET_SIZE);
	memset(wallet[1], 0, MAX_WALLET_SIZE);
	memset(password[0], 0, MAX_WALLET_SIZE);
	memset(password[1], 0, MAX_WALLET_SIZE);
	current_index = 1;
	decodeConfig(cpuMiner);
	cryptoType[0] = cpuMiner.type;
	current_index = 0;
	dpool = now() - TimeRotate * 0.01*(float)(rand()%100);
}

void closeNetwork() {
	stopRequested = true;
}

int getCurrentPool() {
	return current_index;
}