/*=============================================================================

    lichess.cpp  -  Copyright (C) 1999 by Don Cross <dcross@intersrv.com>

    Linux version of InternetChessPlayer.
    Allows two instances of Chenard to play chess over TCP/IP using WinSock.

=============================================================================*/
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "chess.h"
#include "lichess.h"

#define INVALID_SOCKET  (-1)


bool NetworkInitFlag = false;
char MyHostName [256];
hostent MyHostInfo;


int InitializeNetwork ( int &mySocket )
{
    if ( NetworkInitFlag )
        return 1;

    mySocket = INVALID_SOCKET;

    if ( gethostname(MyHostName,sizeof(MyHostName)) != 0 )
    {
        fprintf ( stderr, "Could not determine local host name!  errno=%d\n\n",
                  errno );

        return 0;
    }

    printf ( "Local host name = '%s'\n", MyHostName );
    hostent *myHostInfo = gethostbyname (MyHostName);
    if ( !myHostInfo )
    {
        fprintf ( stderr,
                  "Could not obtain local host information!  errno=%d\n\n",
                  errno );

        return 0;
    }

    MyHostInfo = *myHostInfo;
    mySocket = socket ( AF_INET, SOCK_STREAM, 0 );
    if ( mySocket == INVALID_SOCKET )
    {
        fprintf ( stderr,
                  "Error %d creating socket\n",
                  errno );

        return 0;
    }

    return 1;
}


unsigned InternetChessPlayer::ServerPortNumber = DEFAULT_CHENARD_PORT;


void InternetChessPlayer::SetServerPortNumber ( unsigned newPortNumber )
{
    ServerPortNumber = newPortNumber;
}


unsigned InternetChessPlayer::QueryServerPortNumber()
{
    return ServerPortNumber;
}


InternetChessPlayer::InternetChessPlayer ( ChessUI &ui ):
    ChessPlayer (ui),
    commSocket ( INVALID_SOCKET ),
    listenSocket ( INVALID_SOCKET )
{
    SetQuitReason(qgr_lostConnection);
}


InternetChessPlayer::~InternetChessPlayer()
{
    if ( commSocket != INVALID_SOCKET )
    {
        ::close ( commSocket );
        commSocket = INVALID_SOCKET;
    }

    if ( listenSocket != INVALID_SOCKET )
    {
        ::close ( listenSocket );
        listenSocket = INVALID_SOCKET;
    }
}


int InternetChessPlayer::serverConnect ( ChessSide remoteSide )
{
    if ( !InitializeNetwork (listenSocket) )
        return 0;

    printf ( "Successfully opened socket %d\n", listenSocket );

    sockaddr_in localAddress;
    memset ( &localAddress, 0, sizeof(localAddress) );
    localAddress.sin_family = AF_INET;
    localAddress.sin_addr.s_addr = INADDR_ANY;
    localAddress.sin_port = htons(InternetChessPlayer::QueryServerPortNumber());
    
    int rc = bind ( 
        listenSocket, 
        (sockaddr *) &localAddress, 
        sizeof(localAddress) ); 

    if ( rc != 0 )
    {
        fprintf ( stderr, "Error:  Could not bind socket!\n\n" );
        return 0;
    }

    printf ( "Inform opponent that server address is:  %u.%u.%u.%u\n",
        unsigned ( MyHostInfo.h_addr[0] & 0xff ),
        unsigned ( MyHostInfo.h_addr[1] & 0xff ),
        unsigned ( MyHostInfo.h_addr[2] & 0xff ),
        unsigned ( MyHostInfo.h_addr[3] & 0xff ) );

    rc = listen ( listenSocket, 16 );
    if ( rc != 0 )
    {
        fprintf(stderr, "listen() returned %d, errno=%d\n\n", rc, errno);
        return 0;
    }

    sockaddr connectAddr;
    socklen_t connectAddrSize = sizeof(connectAddr);

    commSocket = accept ( listenSocket, &connectAddr, &connectAddrSize );

    if ( commSocket == INVALID_SOCKET )
    {
        fprintf(stderr, "Error %d accepting connection from listening socket!\n", errno);
        return 0;
    }

    // Send player definition data to client.
    // Express the sides from *their* point of view...

    UINT32 packetSize = 10;
    rc = ::send ( commSocket, (const char *) &packetSize, 4, 0 );
    if ( rc != 4 )
    {
        fprintf ( stderr, "Error %d sending connection packet size!\n", errno );
        return 0;
    }

    rc = ::send ( commSocket, "players ", 8, 0 );
    if ( rc != 8 )
    {
        fprintf ( stderr, "Error %d sending 'players ' string!\n", errno );
        return 0;
    }

    char playerDef[2];
    playerDef[0] = (remoteSide == SIDE_WHITE) ? 'H' : 'I';
    playerDef[1] = (remoteSide == SIDE_WHITE) ? 'I' : 'H';

    rc = ::send ( commSocket, playerDef, 2, 0 );
    if ( rc != 2 )
    {
        fprintf ( stderr, "Error %d sending player definitions!\n", errno );
        return 0;
    }

    printf ( "Successfully established connection with remote client!\n\n" );

    return 1;
}


bool InternetChessPlayer::GetMove (
    ChessBoard &board,
    Move &move,
    INT32 &timeSpent )
{
    timeSpent = 0;
    const INT32 startTime = ChessTime();

    if ( !send(board) )
        return false;

    if ( !receive(board,move) )
        return false;

    timeSpent = ChessTime() - startTime;

    userInterface.DisplayMove ( board, move );
    return true;
}


bool InternetChessPlayer::send ( const ChessBoard &board )
{
    char tempString [256];

    UINT32 numPlies = board.GetCurrentPlyNumber();
    if ( numPlies > 0 )
    {
        // Before receiving the move from the remote opponent, we must
        // send him the complete state of the game as it exists locally.

        // Send an 8-byte string to specify what kind of message this is.
        // In this case, it is "history", because we are sending the entire
        // history of moves in the game history so far...

        UINT32 plyBytes = numPlies * sizeof(Move);
        UINT32 packetSize = 8 + sizeof(numPlies) + plyBytes;

        int result = ::send ( commSocket, (const char *)&packetSize, 4, 0 );

        if ( result != 4 )
        {
            sprintf ( tempString, "send psize: %d", errno );
            userInterface.ReportSpecial (tempString);
            return false;
            sprintf ( tempString, "send psize: %d", errno );
        }

        result = ::send ( commSocket, "history ", 8, 0 );
        if ( result != 8 )
        {
            sprintf ( tempString, "send 'history': %d", errno );
            userInterface.ReportSpecial (tempString);
            return false;
        }

        result = ::send ( commSocket, (const char *)&numPlies, 4, 0 );

        if ( result != 4 )
            return false;

        Move *history = new Move [numPlies];
        if ( !history )
        {
            userInterface.ReportSpecial ( "out of memory!" );
            return false;
        }

        for ( UINT32 ply = 0; ply < numPlies; ++ply )
            history[ply] = board.GetPastMove (ply);

        result = ::send ( commSocket, (const char *)history, plyBytes, 0 );

        if ( UINT32(result) != plyBytes )
        {
            sprintf ( tempString, "send: %d", errno );
            userInterface.ReportSpecial (tempString);
            return false;
        }

        delete[] history;
    }

    return true;
}


bool InternetChessPlayer::receive ( ChessBoard &board, Move &move )
{
    char tempString [256];
    for(;;)
    {
        UINT32 packetSize = 0;
        int result = ::recv ( commSocket, (char *)&packetSize, 4, 0 );

        if ( result != 4 )
        {
            sprintf ( tempString, "recv psize: %d", errno );
            userInterface.ReportSpecial (tempString);
            return false;
        }

        char inMessageType [16];
        memset ( inMessageType, 0, sizeof(inMessageType) );
        result = ::recv ( commSocket, inMessageType, 8, 0 );
        if ( result != 8 )
        {
            sprintf ( tempString, "recv(message): %d", errno );
            userInterface.ReportSpecial (tempString);
            return false;
        }

        if ( strcmp ( inMessageType, "history " ) == 0 )
        {
            // Receive the number of plies from the other side...

            int numPlies = 0;
            result = ::recv ( commSocket, (char *)&numPlies, sizeof(int), 0 );

            if ( result != sizeof(int) )
            {
                sprintf ( tempString, "recv(numPlies): size=%d err=%d",
                          result, errno );

                userInterface.ReportSpecial (tempString);
                return false;
            }

            if ( numPlies > 0 )
            {
                if ( numPlies > 1024 )
                {
                    sprintf ( tempString, "numPlies = %d", numPlies );
                    userInterface.ReportSpecial (tempString);
                    return false;
                }

                Move *history = new Move [numPlies];
                if ( !history )
                {
                    userInterface.ReportSpecial ( "out of memory!" );
                    return false;
                }

                int plyBytes = numPlies * sizeof(Move);
                result = ::recv ( commSocket, (char *)history, plyBytes, 0 );

                if ( result != plyBytes )
                {
                    sprintf ( tempString, "recv: size=%d err=%d",
                              result, errno );
                    userInterface.ReportSpecial (tempString);
                    return false;
                }

                // Now that we safely have the game history from the opponent,
                // we can reset the board and apply all but the final ply to
                // the board.

                // The final ply is returned as the move made by the
                // InternetChessPlayer so that it can be animated on
                // the board display.

                UnmoveInfo unmove;
                board.Init();
                for ( int ply = 0; ply < numPlies-1; ++ply )
                {
                    Move hm = history[ply];
                    if ( (hm.dest & SPECIAL_MOVE_MASK) == SPECIAL_MOVE_EDIT )
                    {
                        board.EditCommand ( hm );
                        board.SaveSpecialMove ( hm );
                    }
                    else
                        board.MakeMove ( hm, unmove );
                }

                move = history [numPlies - 1];
                delete[] history;
            }

            break;
        }
        else if ( strcmp ( inMessageType, "resign  " ) == 0 )
        {
            SetQuitReason(qgr_resign);
            return false;   // remote player has resigned
        }
        else  // unknown message, perhaps from a later version of Chenard
        {
            // eat all the bytes in the message and ignore them

            int bytesLeft = int(packetSize) - 8;
            char trash;
            while ( bytesLeft-- > 0 )
                ::recv ( commSocket, &trash, 1, 0 );
        }
    }

    return true;
}


void InternetChessPlayer::InformResignation()
{
    UINT32 packetSize = 8;
    ::send ( commSocket, (const char *)&packetSize, 4, 0 );
    ::send ( commSocket, "resign  ", 8, 0 );
}


void InternetChessPlayer::InformGameOver ( const ChessBoard &board )
{
    send (board);
}


/*
    $Log: lichess.cpp,v $
    Revision 1.2  2006/01/18 19:58:12  dcross
    I finally got around to scrubbing out silly cBOOLEAN, cFALSE, and cTRUE.
    Now use C++ standard bool, true, false (none of which existed when I started this project).

    Revision 1.1  2005/11/25 19:47:27  dcross
    Recovered lots of old chess source files from the dead!
    Found these on CDROM marked "14 September 1999".
    Added cvs log tag and moved old revision history after that.


    Revision history:

1999 January 23 [Don Cross]
    Porting from WinSock version (ichess.cpp) to Linux environment.

1999 January 5 [Don Cross]
    Started writing.

*/

