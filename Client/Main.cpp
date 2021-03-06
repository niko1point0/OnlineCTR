// Winsock tutorial
// https://docs.microsoft.com/en-us/windows/win32/winsock/finished-server-and-client-code

#define WIN32_LEAN_AND_MEAN

#define ADV_HUB_TEST 0

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

#include <time.h>
#include <stdio.h>

#include "Scanner.h"

// Can be negative
long long int baseAddress;
HANDLE handle;

int currButton = 0;
int prevButton = 0;
bool isHost = false;

// These should be in a config struct
short inCharSelection = 0;
bool inTrackSelection = false;

#define TEST_DEBUG 0

// one short, for controller input
const int Type3_Size = 7;

struct Message
{
	// [server -> client]
	// 0 for track index
	//		track, driver index, num characters, characters
	// 1 for lap index
	//		row index
	// 2 for Start Loading
	//		[null]

	// [bidirectional]
	// 3 for controller input
	//		2 bytes of buttons

	// [client -> server]
	// 4 for kart IDs
	//		client character

	// [bidirectional]
	// 5 for start-line sync
	//		[null]

	unsigned char type;
	unsigned char size;

	// Server and client MUST match
	char data[Type3_Size * 3];
};

struct SocketCtr
{
	SOCKET socket;
	Message sendBuf;
	Message sendBufPrev;
	Message recvBuf;
	Message recvBufPrev;

	unsigned short controllerInput;
};

SocketCtr CtrMain;
int receivedByteCount = 0;
bool inGame = false;
short characterIDs[8];
bool startLine_wait = true;
#define MAX_PLAYERS 4
bool trackSel_wait = true;

unsigned int AddrP1 = 0;
unsigned char gameStateCurr;
unsigned char trackID;
unsigned char numPlayers = 0;
unsigned char myDriverIndex = 0; // will never change on server

int iterationCounter = 0;

// Dont start doing anything till you get input
int framesIdle = 999;

void WriteMem(unsigned int psxAddr, void* pcAddr, int size)
{
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + psxAddr), pcAddr, size, 0);
}

void ReadMem(unsigned int psxAddr, void* pcAddr, int size)
{
	ReadProcessMemory(handle, (PBYTE*)(baseAddress + psxAddr), pcAddr, size, 0);
}

// copied from here https://stackoverflow.com/questions/5891811/generate-random-number-between-1-and-3-in-c
int roll(int min, int max)
{
	// x is in [0,1[
	double x = rand() / static_cast<double>(RAND_MAX + 1);

	// [0,1[ * (max - min) + min is in [min,max[
	int that = min + static_cast<int>(x * (max - min));

	return that;
}

void
init_sockaddr(struct sockaddr_in* name,
	const char* hostname,
	unsigned short port)
{
	struct hostent* hostinfo;

	name->sin_family = AF_INET;
	name->sin_port = htons(port);
	
	hostinfo = gethostbyname(hostname);
	
	if (hostinfo == NULL)
	{
		printf("Unknown host\n");
	}

	name->sin_addr = *(struct in_addr*) hostinfo->h_addr;

	printf("URL converts to IP: %d.%d.%d.%d\n",
		name->sin_addr.S_un.S_un_b.s_b1,
		name->sin_addr.S_un.S_un_b.s_b2,
		name->sin_addr.S_un.S_un_b.s_b3,
		name->sin_addr.S_un.S_un_b.s_b4);
}

void initialize()
{
	int choice = 0;
	HWND console = GetConsoleWindow();
	RECT r;
	GetWindowRect(console, &r); //stores the console's current dimensions

	const int winW = TEST_DEBUG ? 800 : 400;

	// 300 + height of bar (35)
	MoveWindow(console, r.left, r.top, winW, 240+35, TRUE);

	// Initialize random number generator
	srand((unsigned int)time(NULL));

	printf("\n");
	printf("Step 1: Open any ps1 emulator\n");
	printf("Step 2: Open CTR SCUS_94426\n");
	printf("Step 3: Go to Time Trial from main menu\n");
	printf("Step 4: Save state, then load it, (required)\n");
	printf("\n");
	printf("Step 5: Enter emulator PID from 'Details'\n");
	printf("           tab of Windows Task Manager\n");
	printf("Enter: ");

	DWORD procID = 0;
	scanf("%d", &procID);

	printf("\n");
	printf("Searching for CTR 94426 in emulator ram...\n");

	// open the process with procID, and store it in the 'handle'
	handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procID);

	// if handle fails to open
	if (!handle)
	{
		printf("Failed to open process\n");
		system("pause");
		exit(0);
	}

	// This idea to scan memory for 11 bytes to automatically
	// find that CTR is running, and to find the base address
	// of any emulator universally, was EuroAli's idea in the
	// CTR-Tools discord server. Thank you EuroAli

	// Shows at PSX address 0x8003C62C, only in CTR 94426
	unsigned char ctrData[12] = { 0x71, 0xDC, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xD0, 0xF9, 0x00, 0x0C };

	// can't be nullptr by default or it crashes,
	// it will become 1 when the loop starts
	baseAddress = 0;

	// Modified from https://guidedhacking.com/threads/hyperscan-fast-vast-memory-scanner.9659/
	std::vector<UINT_PTR> AddressHolder = Hyperscan::HYPERSCAN_SCANNER::Scan(procID, ctrData, 12, Hyperscan::HyperscanAllignment4Bytes,
		Hyperscan::HyperscanTypeExact);

	// take the first (should be only) result
	baseAddress = AddressHolder[0];

	// Remove 0x8003C62C address of PSX memory,
	// to find the relative address where PSX memory
	// is located in RAM. It is ok for baseAddress
	// to be a negative number
	baseAddress -= 0x8003C62C;

	// name of the server that you connect to
	char* serverName = nullptr;

	// set bool
	// set max variables
	// get server name
	system("cls");
	printf("Enter IP or URL: ");
	serverName = (char*)malloc(80);
	scanf("%79s", serverName);

	system("cls\n");

	WSADATA wsaData;
	int iResult;
	struct addrinfo* result = NULL,
		* ptr = NULL,
		hints;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		system("pause");
		exit(0);
	}

	// sockAddr
	struct sockaddr_in socketIn;
	init_sockaddr(&socketIn, serverName, 1234);

	// Create a SOCKET for connecting to server
	CtrMain.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// Setup the TCP listening socket
	int res = connect(CtrMain.socket, (struct sockaddr*) & socketIn, sizeof(socketIn));

	freeaddrinfo(result);

	if (CtrMain.socket == INVALID_SOCKET) {
		printf("Unable to connect to server!\n");
		WSACleanup();
		system("pause");
		exit(0);
	}

	// set socket to non-blocking
	unsigned long nonBlocking = 1;
	iResult = ioctlsocket(CtrMain.socket, FIONBIO, &nonBlocking);

	printf("Connected to server\n\n");

	printf("In Character Selection\n");
	printf("Press L2 for Oxide\n");
	printf("Press R2 for Random\n");
	printf("\n");
}

// globals
unsigned char type = 0xFF;
unsigned char size = 0xFF;

void RecvTrackMessage()
{
	if (size == 2)
	{
		isHost = true;
		printf("You are host, so you pick track\n");
		printf("Press L2 for Battle Maps\n");
		printf("Press R2 for Random Pick\n");
		return;
	}

	unsigned short twoBytes[2];
	memcpy(twoBytes, CtrMain.recvBuf.data, 4);

	// 3 bytes, 24 bits
	// >> 0 -- charID 1
	// >> 4 -- charID 2
	// >> 8 -- charID 3
	// >> 12 -- lap (0,1,2)
	// >> 14 -- everyoneReady?
	// >> 15 -- race starting?

	// >> 16 lapRow open?
	// >> 17 trackID
	// >> 22 clientID
	// >> 24 numPlayers

	numPlayers = CtrMain.recvBuf.data[3];

	// First two bytes are characters
	// Get characterID for this player
	// for characters 0 - 7:
	// CharacterID[i] : 0x1608EA4 + 2 * i
	for (int i = 0; i < numPlayers-1; i++)
	{
		characterIDs[i + 1] = (twoBytes[0] >> (4*i) & 0b1111);
	}

	char lapRow = (twoBytes[0] >> 12) & 0b11;
	char everyoneReady = (twoBytes[0] >> 14) & 1;
	char raceStarting = (twoBytes[0] >> 15) & 1;

	char trackByte = (twoBytes[1]) & 0b11111;
	char lapRowOpen = (twoBytes[1] >> 5) & 1;
	char myDriverIndex = (twoBytes[1] >> 6) & 0b11;

	char ogTrackByte = 0;

	if(!everyoneReady)
	{
		// set controller mode to 0P mode, trigger error message
		char _0 = 0;
		WriteMem(0x800987C9, &_0, sizeof(_0));

		// change the error message
		WriteMem(0x800BC684, (char*)"waiting for players...", 23);
	}

	else
	{
		// set controller mode to 1P mode, remove error message
		char _1 = 1;
		WriteMem(0x800987C9, &_1, sizeof(_1));
	}

	// skip menu sync if you are host
	if (isHost) return;

	// Get original track byte
	ReadMem(0x800B46FA, &ogTrackByte, sizeof(char));

	if (trackByte != ogTrackByte)
	{
		// set Text+Map address 
		WriteMem(0x800B46FA, &trackByte, sizeof(char));

		// set Video Address
		WriteMem(0x800B59A8, &trackByte, sizeof(char));

		// Set two variables to refresh the video
		short s_One = 1;
		WriteMem(0x800B59B8, &s_One, sizeof(short));
		WriteMem(0x800B59BA, &s_One, sizeof(short));
	}

	// Set status of lapRow
	WriteMem(0x800B59AC, &lapRowOpen, sizeof(char));

	if (lapRowOpen)
	{
		// convert to one byte
		WriteMem(0x8008D920, &lapRow, sizeof(lapRow));

#if TEST_DEBUG
		printf("Recv -- Tag: %d, size: %d, -- %d\n", type, size, lapByte);
#endif
		// change the spawn order

		// Client 0:       0 1 2 3 4 5 6 7
		// Client 1: 1 0       2 3 4 5 6 7
		// Client 2: 2 0 1       3 4 5 6 7
		// Client 3: 3 0 1 2       4 5 6 7

		// used to set your own spawn
		char zero = 0;

		// loop through all drivers prior to your index
		for (char i = 1; i <= myDriverIndex; i++)
		{
			char spawnValue = i - 1;

			WriteMem(0x80080F28 + i, &spawnValue, sizeof(char));
		}

		// set your own index
		WriteMem(0x80080F28, &myDriverIndex, sizeof(char));
	
	}

	if (raceStarting)
	{
		char one = 1;
		char two = 2;

		// set menuA to 2 and menuB to 1,
		WriteMem(0x800B59AE, &two, sizeof(char));
		WriteMem(0x800B59B0, &one, sizeof(char));

		// Reset game frame counter to zero
		int zero = 0;
		WriteMem(0x80096B20 + 0x1cec, &zero, sizeof(int));

		inGame = false;
		startLine_wait = true;
	}
}

void RecvPosMessage()
{
#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d, -- ", type, size);
	
	for (int i = 0; i < numPlayers - 1; i++)
	{
		printf("%04X ",
			*(short*)&CtrMain.recvBuf.data[Type3_Size * i + 0]);
	}
	printf("\n");
#endif

	// This NEEDS to move somewhere else

	// If race is ready to start
	if (!startLine_wait)
	{
		// draw all AIs
		for (int i = 0; i < numPlayers - 1; i++)
		{
			WriteMem(0x80096804 + 0x50 * (i + 1) + 0x10, &CtrMain.recvBuf.data[Type3_Size * i + 0], sizeof(short));

			// offset 6 has 8 bits
			// unused
			// unused
			// 2b iterator
			// 4b weapon

			int netIterationCounter = (char)CtrMain.recvBuf.data[Type3_Size * i + 6] >> 4;

			int AddrNetPlayer = AddrP1 + 0x670 * (i+1);

			// X position
			if (netIterationCounter == 0)
			{
				WriteMem(AddrNetPlayer + 0x2D4, &CtrMain.recvBuf.data[Type3_Size * i + 2], sizeof(int));
			}

			// Y position
			if (netIterationCounter == 1)
			{
				WriteMem(AddrNetPlayer + 0x2D8, &CtrMain.recvBuf.data[Type3_Size * i + 2], sizeof(int));
			}

			// Z position
			if (netIterationCounter == 2)
			{
				WriteMem(AddrNetPlayer + 0x2DC, &CtrMain.recvBuf.data[Type3_Size * i + 2], sizeof(int));
			}

			// rotation
			if (netIterationCounter == 3)
			{
												// rotation, so it's short
				WriteMem(AddrNetPlayer + 0x39A, &CtrMain.recvBuf.data[Type3_Size * i + 2], sizeof(short));
			}

			char weapon = CtrMain.recvBuf.data[Type3_Size * i + 6] & 0xf;
			WriteMem(AddrNetPlayer + 0x36, &weapon, sizeof(char));

			// Dont forget to handle weapon
			// And also the '5' message needs the game clock
		}
	}
}

void RecvCharacterMesssage()
{
	// server will not send this message
}

void RecvStartRaceMessage()
{
#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d\n", type, size);
#endif

	startLine_wait = false;

	// set controller mode to 1P, remove error message
	char _1 = 1;
	WriteMem(0x800987C9, &_1, sizeof(_1));
}

void (*RecvMessage[6]) () =
{
	RecvTrackMessage,
	nullptr,
	nullptr,
	RecvPosMessage,
	RecvCharacterMesssage,
	RecvStartRaceMessage
};

void Disconnect()
{
	system("cls");
	printf("Connection Lost\n\n");
	printf("1. Close Client.exe\n");
	printf("2. Load your save state\n");
	printf("3. Open Client.exe and reconnect\n");

	while (true)
	{
		// set controller mode to 0P mode, trigger error message
		char _0 = 0;
		WriteMem(0x800987C9, &_0, sizeof(_0));

		// change the error message
		WriteMem(0x800BC684, (char*)"Connection Lost", 23);

		Sleep(1);
	}
}

void updateNetwork()
{
	// Get a message
	memset(&CtrMain.recvBuf, 0xFF, sizeof(Message));
	receivedByteCount = recv(CtrMain.socket, (char*)&CtrMain.recvBuf, sizeof(Message), 0);
	
	// check for errors
	if (receivedByteCount == -1)
	{
		int err = WSAGetLastError();

//#if TEST_DEBUG
		// This happens due to nonblock, ignore it
		if (err != WSAEWOULDBLOCK)
		{
			printf("Error %d\n", err);
		}
//#endif

		// if server is closed disconnected
		if (err == WSAECONNRESET)
		{
			Disconnect();
		}

		if (err == WSAENOTCONN)
		{
			system("cls");
			printf("Failed to connect to server\n\n");
			printf("Close Client.exe and reopen, try again\n");

			printf("\n");
			system("pause");
			exit(0);
		}

		goto SendToServer;
	}
	
	// This happens when the server uses closesocket(),
	// either because you connected to a full server, or
	// a client disconnected so the server reset
	if (receivedByteCount == 0)
	{
		Disconnect();
	}
	
	if (receivedByteCount < CtrMain.recvBuf.size)
	{
		//printf("Bug! -- Tag: %d, recvBuf.size: %d, recvCount: %d\n",
		//	recvBuf.type, recvBuf.size, receivedByteCount);
	
		goto SendToServer;
	}
	
	// We can confirm we have a valid message
	
	// dont parse same message twice
	if (CtrMain.recvBuf.size == CtrMain.recvBufPrev.size)
		if (memcmp(&CtrMain.recvBuf, &CtrMain.recvBufPrev, CtrMain.recvBuf.size) == 0)
			goto SendToServer;
	
	// make a backup
	memcpy(&CtrMain.recvBufPrev, &CtrMain.recvBuf, sizeof(Message));
	
	type = CtrMain.recvBuf.type;
	size = CtrMain.recvBuf.size;
	
	// execute message depending on type
	if(type >= 0 && type <= sizeof(RecvMessage)/sizeof(RecvMessage[0]))
		RecvMessage[type]();
	
SendToServer:
	
	if (CtrMain.sendBuf.type == 0xFF)
		return;
	
	// dont send the same message twice, 
	// or
	// To do: if server has not gotten prev message
	if (CtrMain.sendBuf.size == CtrMain.sendBufPrev.size)
		if (memcmp(&CtrMain.sendBuf, &CtrMain.sendBufPrev, CtrMain.sendBuf.size) == 0)
			return;
	
	// send a message to the client
	send(CtrMain.socket, (char*)&CtrMain.sendBuf, sizeof(Message), 0);
	
	// make a backup
	memcpy(&CtrMain.sendBufPrev, &CtrMain.sendBuf, sizeof(Message));
	
#if TEST_DEBUG

	type = CtrMain.sendBuf.type;
	size = CtrMain.sendBuf.size;
	
	if (type == 3)
	{
		short s1 = *(int*)&CtrMain.sendBuf.data[0];
	
		printf("Send -- Tag: %d, size: %d, -- %04X \n", type, size, s1);
	}
	
	if (type == 4)
	{
		// parse message
		char c1 = CtrMain.sendBuf.data[0];
	
		printf("Send -- Tag: %d, size: %d, -- %d\n", type, size, c1);
	}
	
	if (type == 5)
	{
		printf("Send -- Tag: %d, size: %d\n", type, size);
	}
#endif
}

void SendOnlinePlayersToRAM()
{
	// 0xffff (-1) spawns one player
	// 0 spawns 2 players
	// 1 spawns 3 players, etc
	if (numPlayers != 1)
	{
		short playerMin2 = numPlayers - 2;
		WriteMem(0x8003B750, &playerMin2, sizeof(playerMin2));
	}

	// Value 8 by default, which spawns 8 racers
	// As 1, or anything else, it removes AIs
	char one = 1;
	WriteMem(0x8003B83C, &one, sizeof(one));

	// set number of icons (on the left of the screen)
	if(numPlayers < 4)
		WriteMem(0x800525A8, &numPlayers, sizeof(numPlayers));

	// put network characters into RAM
	for (unsigned char i = 1; i < numPlayers; i++)
	{
		// Set character IDs
		char oneByte = (char)characterIDs[(int)i];
		WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts

		// No clue why this fails
#if 0
		// Remove blinking on map for online players
		int flags = 0;
		ReadMem(AddrP1 + 0x670 * i + 0x2c8, &flags, sizeof(flags));
		flags = flags & 0xFFEFFFFF;
		WriteMem(AddrP1 + 0x670 * i + 0x2c8, &flags, sizeof(flags));
#endif
	}
}

void HostShortcutKeys()
{
	int L2 = 0x100;
	int R2 = 0x200;
	prevButton = currButton;
	ReadMem(0x8008d974, &currButton, 4);

	bool tapL2 = !(prevButton & L2) && (currButton & L2);
	bool tapR2 = !(prevButton & R2) && (currButton & R2);

	if (tapL2 || tapR2)
	{
		char trackByte = tapL2 ? 24 : (char)roll(0, 17);

		// set Text+Map address 
		WriteMem(0x800B46FA, &trackByte, sizeof(char));

		// set Video Address
		WriteMem(0x800B59A8, &trackByte, sizeof(char));

		// Set two variables to refresh the video
		short s_One = 1;
		WriteMem(0x800B59B8, &s_One, sizeof(short));
		WriteMem(0x800B59BA, &s_One, sizeof(short));
	}
}

void GetHostMenuState()
{
	// check if lapRowSelector is open
	char lapRowSelectorOpen = 0;
	ReadMem(0x800B59AC, &lapRowSelectorOpen, sizeof(bool));

	// battle maps and random maps
	HostShortcutKeys();

	// Get Track ID, send it to clients
	ReadMem(0x800B46FA, &trackID, sizeof(trackID));

	// 0 -> 3 laps
	// 1 -> 5 laps
	// 2 -> 7 laps
	unsigned char lapRowSelected = 0;
	ReadMem(0x8008D920, &lapRowSelected, sizeof(lapRowSelected));

	
	CtrMain.sendBuf.type = 0;
	CtrMain.sendBuf.size = 5;

	CtrMain.sendBuf.data[0] = trackID;
	CtrMain.sendBuf.data[1] = (unsigned char)characterIDs[0];
	CtrMain.sendBuf.data[2] = lapRowSelected;

	// These determine if the loading screen has triggered yet
	unsigned char menuA = 0;
	unsigned char menuB = 0;
	ReadMem(0x800B59AE, &menuA, sizeof(menuA));
	ReadMem(0x800B59B0, &menuB, sizeof(menuB));

	// if race is starting
	if (menuA == 2 && menuB == 1)
	{
		// wait for all at starting line
		startLine_wait = true;

		// Reset game frame counter to zero
		int zero = 0;
		WriteMem(0x80096B20 + 0x1cec, &zero, sizeof(int));

		inGame = false;

		CtrMain.sendBuf.data[2] += 1 << 2;
	}
	
	CtrMain.sendBuf.data[2] += lapRowSelectorOpen << 3;
}

void SendCharacterID()
{
	CtrMain.sendBuf.type = 4;
	CtrMain.sendBuf.size = 3;
	CtrMain.sendBuf.data[0] = (char)characterIDs[0];
}

void SwapModes()
{
	// Get the game mode
	unsigned int gameMode = 0;
	ReadMem(0x80096B20, &gameMode, sizeof(int));

	// Disable Time Trial (0x20000)
	gameMode = gameMode & 0xFFFDFFFF;

	// Enable Arcade
	gameMode = gameMode | 0x400000;

	// Write mode
	WriteMem(0x80096B20, &gameMode, sizeof(int));

	// if you're in main menu
	if (gameMode & 0x2000)
	{
		char name[7] = "Arcade";
		WriteMem(0x800BCC59, name, 7);
	}

	else
	{
		char name[7] = "Online";
		WriteMem(0x800BCC59, name, 7);
	}
}

void HandleInjectionASM()
{
	// test to see if someone loaded a save state

	// If you're ingame, you probably saved a state with injection
	if (inGame)
		return;

	// Do this regardless if ASM already injected,
	// just in case someone left time trial and went back
	SwapModes();

	// If you're not ingame, your state might not have injection
	short TestHighMpk;
	short HighMpk = 0x00F2;
	ReadMem(0x80032888, &TestHighMpk, sizeof(short));

	// If you already have the injection, you're good to go
	if (TestHighMpk == HighMpk)
		return;

	// Unlock all cars and tracks immediately
	unsigned long long value = 0xFFFFFFFFFFFFFFFF;
	WriteMem(0x8008E6EC, &value, sizeof(value));

	// Ja ra, return asm, 
	// disable weapons for players and enemies
	// This is only used for player, becuase enemies are
	// now disabled by locking their 0x624 offset to 30
	int jaRa = 0x3e00008;
	WriteMem(0x8006540C, &jaRa, sizeof(int));

	int zero = 0;

#if ADV_HUB_TEST == 0
	// Patch the first if-statement of FUN_8003282c
	// Allow 4 characters to load in high LOD
	WriteMem(0x80032840, &zero, sizeof(int));

	// Only first 3 characters are high lod
	// Leave 4th as low, or everything breaks
	WriteMem(0x80032888, &HighMpk, sizeof(short));
	WriteMem(0x800328A4, &HighMpk, sizeof(short));
	WriteMem(0x800328C0, &HighMpk, sizeof(short));
#endif

	// Disable collision between players
	WriteMem(0x80042368, &zero, sizeof(int));

	// NOP instructions that recursively 
	// loop through all players to draw all HUDs

	// Fix 2D HUD (lap, powerslide meter)
	WriteMem(0x80053B8C, &zero, sizeof(zero));

	// Fix 3D HUD (wumpa fruit)
	WriteMem(0x8004CE88, &zero, sizeof(zero));

	// Erase controller loop, so it doesn't
	// overwrite the controls I inject, with 
	// data from emulated controllers
	WriteMem(0x80025844, &zero, sizeof(zero));
}

void HandleCharacterSelection()
{
	ReadMem(0x8008D908, &inCharSelection, sizeof(inCharSelection));

	// if you are not in character selection menu
	if (inCharSelection != 18100)
		return;

	char penta = 0xD;
	char oxide = 0xF;

	// If you character ID is 0xF, then
	// send cursor to penta's icon buffer.
	// By default, it goes to Crash
	WriteMem(0x800b50d2, &penta, 1);

	// Check if Penta slot is set to penta icon or oxide icon
	char currID;
	ReadMem(0x800B4F24, &currID, 1);

	// Handle curr and prev button
	int L2 = 0x100;
	int R2 = 0x200;
	prevButton = currButton;
	ReadMem(0x8008d974, &currButton, 4);

	// Check for tapping L2 or R2
	bool tapL2 = !(prevButton & L2) && (currButton & L2);
	bool tapR2 = !(prevButton & R2) && (currButton & R2);

	// Find every place where character ID is stored
	char a;
	char b;
	char c;
	char d;
	ReadMem(0x80086E84, &a, sizeof(char));
	ReadMem(0x800B59F0, &b, sizeof(char));
	ReadMem(0x800B59F8, &c, sizeof(char));
	ReadMem(0x801FFEA8, &d, sizeof(char));

	// Find if any of them are oxide
	char oxideAnywhere = (a == 15 || b == 15 || c == 15 || d == 15);

	// hide screen
	char _10 = oxideAnywhere * 0x10;
	WriteMem(0x800B4D45, &_10, 1);

	// change icon
	if (tapL2)
	{
		tapL2 = false;
		WriteMem(0x800B4F24, (currID == penta) ? &oxide : &penta, 1);
	}


	// Choose Random Track if you can't decide
	if (tapR2)
	{
		// Get random kart
		char kartByte = (char)roll(0, 0xF);
		characterIDs[0] = kartByte;
		WriteMem(0x80086E84, &kartByte, sizeof(char));
	}
}

void HandleTrackSelection()
{
	// Check to see if you are in the track selection menu
	ReadMem(0x8008D88C, &inTrackSelection, sizeof(inTrackSelection));

	// if you're not in the track selection menu
	if (!inTrackSelection)
		return;

	// Get characterID for this player
	ReadMem(0x80086E84, &characterIDs[0], sizeof(short));

	// wait for all players before continuing
	if (isHost)
	{
		// copy host menu state to clients
		GetHostMenuState();
	}

	else
	{
		// tell the server who you picked
		SendCharacterID();
	}
}

int main(int argc, char** argv)
{
	initialize();

	clock_t start = clock();
	clock_t end = clock();

	// Main loop...
	while (true)
	{
		// end of previous cycle
		end = clock();

		// If you finished in less than 16ms (1/60 second) 
		int ms = end - start;
		if (ms < 16) Sleep(16 - ms);

		// start of next cycle
		start = clock();

		// get address of 0x670-byte racer struct
		ReadMem(0x8009900C, &AddrP1, sizeof(AddrP1));

		// handle all message reading and writing
		updateNetwork();

		// load secret characters, high lods, etc
		HandleInjectionASM();

		// L2 or oxide, R2 for random
		HandleCharacterSelection();

		// send messages to server, whether host or guest,
		// this does not handle recv or injecting track values
		HandleTrackSelection();

		// Test Online in Adventure
		#if ADV_HUB_TEST == 1
			char map = 25;
			WriteMem(0x80096B20 + 0x1EB0, &map, sizeof(map));
		
			// 100% unlock everything
			unsigned char advData[16];
			for (int i = 0; i < 16; i++)
			{
				advData[i] = 0xFF;
			}
		
			WriteMem(0x8008fba4, advData, sizeof(advData));

			// RAM Expansion
			int ramExpand = 0x807FF800;
			WriteMem(0x800990EC, &ramExpand, sizeof(ramExpand));
			WriteMem(0x800990F0, &ramExpand, sizeof(ramExpand));
		#endif

		// GameState
		// 2 = loading screen
		// 10 = some menus, intro of race (including traffic lights)
		// 11 = racing

		// Read gameStateCurr
		ReadMem(0x80098851, &gameStateCurr, sizeof(gameStateCurr));

		// when you're in the loading screen
		if (gameStateCurr == 2)
		{
			// constantly write these values,
			// to make sure the right characters are loaded
			SendOnlinePlayersToRAM();
		}


		// Read Game Timer
		int timer = 0;
		ReadMem(0x80096B20 + 0x1cec, &timer, sizeof(int));

		if (!inGame)
		{
			if (
				timer > 30 &&
				!inTrackSelection &&
				inCharSelection != 18100
				)
			{
				if (gameStateCurr == 11 || gameStateCurr == 10)
				{
					// see if the intro cutscene is playing
					// becomes 0 when traffic lights should show
					char introAnimState;
					ReadMem(0x801FFDDE, &introAnimState, sizeof(char));

					// if the intro animation is done
					if (introAnimState == 0)
					{
						inGame = true;
					}
				}

				#if ADV_HUB_TEST == 1

				else
				{
					if (gameStateCurr == 43)
					{
						inGame = true;
					}
				}

				#endif
			}
		}

		if(inGame)
		{
			// If not all racers are ready to start
			if (startLine_wait)
			{
				// Set the traffic lights to be above the screen
				// They are set to 3840 by default without modding
				short wait = 4500;
				WriteMem(0x8009882C, &wait, sizeof(short));

				// set controller mode to 0P mode, trigger error message
				char _0 = 0;
				WriteMem(0x800987C9, &_0, sizeof(_0));

				// change the error message
				WriteMem(0x800BC684, (char*)"waiting for players...", 23);

				// client "wants" to start
				CtrMain.sendBuf.type = 5;
				CtrMain.sendBuf.size = 2;
			}

			// if everyone is ready to start
			else
			{
				// set controller mode to 1P mode, disable error message
				// The message gets enabled lower in the code
				char _1 = 1;
				WriteMem(0x800987C9, &_1, sizeof(_1));

				int flags;
				ReadMem(0x80096B20, &flags, sizeof(int));

				// if you are still in gameplay,
				// Arcade mode, or Arcade + Intro, or Arcade + Weapon
				if (
						flags == 0x400000 || 
						flags == 0x400040 || 
						flags == 0xC00000
					
						#if ADV_HUB_TEST == 1
						|| flags == 0x500000
						#endif
					)
				{
					CtrMain.sendBuf.type = 3;
					CtrMain.sendBuf.size = Type3_Size+2;

					ReadMem(0x80096804 + 0x50*0 + 0x10, &CtrMain.sendBuf.data[0], sizeof(short));
					
#if 0
					if ((short)CtrMain.sendBuf.data[0] == 0)
					{
						framesIdle++;

						if (framesIdle > 300)
							framesIdle = 160;
					}

					else
					{
						framesIdle = 0;
					}

					// about 3 seconds
					if (framesIdle > 150)
						continue;
#endif

					iterationCounter = iterationCounter & 0b11; // 0 - 3

					// X position
					if (iterationCounter == 0)
					{
						ReadMem(AddrP1 + 0x2D4, &CtrMain.sendBuf.data[2], sizeof(int));
					}

					// Y position
					else if (iterationCounter == 1)
					{
						ReadMem(AddrP1 + 0x2D8, &CtrMain.sendBuf.data[2], sizeof(int));
					}

					// Z position
					else if (iterationCounter == 2)
					{
						ReadMem(AddrP1 + 0x2DC, &CtrMain.sendBuf.data[2], sizeof(int));
					}

					// Rotation
					else if (iterationCounter == 3)
					{
						// really only needs to be 2 bytes, but whatever
						ReadMem(AddrP1 + 0x39A, &CtrMain.sendBuf.data[2], sizeof(int));
					}

					// weapon
					ReadMem(AddrP1 + 0x36, &CtrMain.sendBuf.data[6], sizeof(char));

					// temporary
					// overwrite instead of bit shifting
					CtrMain.sendBuf.data[6] += (iterationCounter << 4);

					iterationCounter++;
				}

				// if you finished or left the race
				else
				{
					inGame = false;
					CtrMain.sendBuf.type = 6;
					CtrMain.sendBuf.size = 2;
				}
			}
		}
	}

	return 0;
}