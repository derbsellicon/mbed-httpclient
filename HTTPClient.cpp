/* HTTPClient.cpp */
/*
Copyright (C) 2012 ARM Limited.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define __DEBUG__ 4 //Maximum verbosity
#ifndef __MODULE__
#define __MODULE__ "HTTPClient.cpp"
#endif

#include "core/fwk.h"

#include "HTTPClient.h"

#define HTTP_REQUEST_TIMEOUT 30000
#define HTTP_PORT 80

#define CHUNK_SIZE 256

#include <cstring>

HTTPClient::HTTPClient() :
m_basicAuthUser(NULL), m_basicAuthPassword(NULL), m_httpResponseCode(0)
{

}

HTTPClient::~HTTPClient()
{

}

#if 0
void HTTPClient::basicAuth(const char* user, const char* password) //Basic Authentification
{
  m_basicAuthUser = user;
  m_basicAuthPassword = password;
}
#endif

int HTTPClient::get(const char* url, IHTTPDataIn* pDataIn, uint32_t timeout /*= HTTP_CLIENT_DEFAULT_TIMEOUT*/) //Blocking
{
  return connect(url, HTTP_GET, NULL, pDataIn, timeout);
}

int HTTPClient::get(const char* url, char* result, size_t maxResultLen, uint32_t timeout /*= HTTP_CLIENT_DEFAULT_TIMEOUT*/) //Blocking
{
  HTTPText str(result, maxResultLen);
  return get(url, &str, timeout);
}

int HTTPClient::post(const char* url, const IHTTPDataOut& dataOut, IHTTPDataIn* pDataIn, uint32_t timeout /*= HTTP_CLIENT_DEFAULT_TIMEOUT*/) //Blocking
{
  return connect(url, HTTP_POST, (IHTTPDataOut*)&dataOut, pDataIn, timeout);
}

int HTTPClient::getHTTPResponseCode()
{
  return m_httpResponseCode;
}


int HTTPClient::connect(const char* url, HTTP_METH method, IHTTPDataOut* pDataOut, IHTTPDataIn* pDataIn, uint32_t timeout) //Execute request
{
  m_httpResponseCode = 0; //Invalidate code
  m_timeout = timeout;

  char scheme[8];
  uint16_t port;
  char host[32];
  char path[64];
  //First we need to parse the url (http[s]://host[:port][/[path]]) -- HTTPS not supported (yet?)
  int ret = parseURL(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path));
  if(ret != OK)
  {
    ERR("parseURL returned %d", ret);
    return ret;
  }

  if(port == 0) //TODO do handle HTTPS->443
  {
    port = 80;
  }

  DBG("Scheme: %s", scheme);
  DBG("Host: %s", host);
  DBG("Port: %d", port);
  DBG("Path: %s", path);

  //Now populate structure
  std::memset(&m_serverAddr, 0, sizeof(struct sockaddr_in));

  //Resolve DNS if needed

  DBG("Resolving DNS address or populate hard-coded IP address");
  struct hostent *server = socket::gethostbyname(host);
  if(server == NULL)
  {
    return NET_NOTFOUND; //Fail
  }
  memcpy((char*)&m_serverAddr.sin_addr.s_addr, (char*)server->h_addr_list[0], server->h_length);

  m_serverAddr.sin_family = AF_INET;
  m_serverAddr.sin_port = htons(port);

  //Create socket
  DBG("Creating socket");
  m_sock = socket::socket(AF_INET, SOCK_STREAM, 0); //UDP socket
  if (m_sock < 0)
  {
    ERR("Could not create socket");
    return NET_OOM;
  }
  DBG("Handle is %d", m_sock);

  //Connect it
  DBG("Connecting socket to %s:%d", inet_ntoa(m_serverAddr.sin_addr), ntohs(m_serverAddr.sin_port));
  ret = socket::connect(m_sock, (const struct sockaddr *)&m_serverAddr, sizeof(m_serverAddr));
  if (ret < 0)
  {
    socket::close(m_sock);
    ERR("Could not connect");
    return NET_CONN;
  }

  //Send request
  DBG("Sending request");
  char line[128];
  const char* meth = (method==HTTP_GET)?"GET":(method==HTTP_POST)?"POST":"";
  snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s\r\n", meth, path, host); //Write request
  ret = send(line);
  if(ret)
  {
    socket::close(m_sock);
    ERR("Could not write request");
    return NET_CONN;
  }

  //Send all headers

  //Send default headers
  DBG("Sending headers");
  if( (method == HTTP_POST) && (pDataOut != NULL) )
  {
    if( pDataOut->getIsChunked() )
    {
      ret = send("Transfer-Encoding: chunked\r\n");
      if(ret != OK) goto connerr;
    }
    else
    {
      snprintf(line, sizeof(line), "Content-Length: %d\r\n", pDataOut->getDataLen());
      ret = send(line);
      if(ret != OK) goto connerr;
    }
    char type[48];
    if( pDataOut->getDataType(type, 48) == OK )
    {
      snprintf(line, sizeof(line), "Content-Type: %s\r\n", type);
      ret = send(line);
      if(ret != OK) goto connerr;
    }
  }

  //Close headers
  DBG("Headers sent");
  ret = send("\r\n");
  if(ret != OK) goto connerr;

  char buf[CHUNK_SIZE];
  size_t trfLen;

  //Send data (if POST)
  if( (method == HTTP_POST) && (pDataOut != NULL) )
  {
    DBG("Sending data");
    while(true)
    {
      size_t writtenLen = 0;
      pDataOut->read(buf, CHUNK_SIZE, &trfLen);
      if( pDataOut->getIsChunked() )
      {
        //Write chunk header
        snprintf(line, sizeof(line), "%X\r\n", trfLen); //In hex encoding
        ret = send(line);
        if(ret != OK) goto connerr;
      }
      else if( trfLen == 0 )
      {
        break;
      }
      if( trfLen != 0 )
      {
        ret = send(buf, trfLen);
        if(ret != OK) goto connerr;
      }

      if( pDataOut->getIsChunked()  )
      {
        ret = send("\r\n"); //Chunk-terminating CRLF
        if(ret != OK) goto connerr;
      }
      else
      {
        writtenLen += trfLen;
        if( writtenLen >= pDataOut->getDataLen() )
        {
          break;
        }
      }

      if( trfLen == 0 )
      {
        break;
      }
    }

  }

  //Receive response
  DBG("Receiving response");
  ret = recv(buf, CHUNK_SIZE, CHUNK_SIZE, &trfLen); //Read n bytes
  if(ret != OK) goto connerr;

  buf[trfLen] = '\0';

  char* crlfPtr = strstr(buf, "\r\n");
  if(crlfPtr == NULL)
  {
    goto prtclerr;
  }

  int crlfPos = crlfPtr - buf;
  buf[crlfPos] = '\0';

  //Parse HTTP response
  if( sscanf(buf, "HTTP/%*d.%*d %d %*[^\r\n]", &m_httpResponseCode) != 1 )
  {
    //Cannot match string, error
    ERR("Not a correct HTTP answer : %s\n", buf);
    goto prtclerr;
  }

  if(m_httpResponseCode != 200)
  {
    //Cannot match string, error
    WARN("Response code %d", m_httpResponseCode);
    goto prtclerr;
  }

  DBG("Reading headers");

  memmove(buf, &buf[crlfPos+2], trfLen - (crlfPos + 2));
  trfLen -= (crlfPos + 2);

  size_t recvContentLength = 0;
  bool recvChunked = false;
  //Now get headers
  while( true )
  {
    crlfPtr = strstr(buf, "\r\n");
    if(crlfPtr == NULL)
    {
      if( trfLen < CHUNK_SIZE )
      {
        size_t newTrfLen;
        ret = recv(buf + trfLen, 1, CHUNK_SIZE - trfLen - 1, &newTrfLen);
        trfLen += newTrfLen;
        buf[trfLen] = '\0';
        DBG("In buf: [%s]", buf);
        if(ret != OK) goto connerr;
        continue;
      }
      else
      {
        goto prtclerr;
      }
    }

    crlfPos = crlfPtr - buf;

    if(crlfPos == 0) //End of headers
    {
      DBG("Headers read");
      memmove(buf, &buf[2], trfLen - 2);
      trfLen -= 2;
      break;
    }

    buf[crlfPos] = '\0';

    char key[16];
    char value[16];

    int n = sscanf(buf, "%16[^:]: %16[^\r\n]", key, value);
    if ( n == 2 )
    {
      DBG("Read header : %s: %s\n", key, value);
      if( !strcmp(key, "Content-Length") )
      {
        sscanf(value, "%d", &recvContentLength);
        pDataIn->setDataLen(recvContentLength);
      }
      else if( !strcmp(key, "Transfer-Encoding") )
      {
        if( !strcmp(value, "Chunked") || !strcmp(value, "chunked") )
        {
          recvChunked = true;
          pDataIn->setIsChunked(true);
        }
      }
      else if( !strcmp(key, "Content-Type") )
      {
        pDataIn->setDataType(value);
      }

      memmove(buf, &buf[crlfPos+2], trfLen - (crlfPos + 2));
      trfLen -= (crlfPos + 2);

    }
    else
    {
      ERR("Could not parse header");
      goto prtclerr;
    }

  }

  //Receive data
  DBG("Receiving data");
  while(true)
  {
    size_t readLen = 0;

    if( recvChunked )
    {
      //Read chunk header
      crlfPos=0;
      for(crlfPos++; crlfPos < trfLen - 2; crlfPos++)
      {
        if( buf[crlfPos] == '\r' && buf[crlfPos + 1] == '\n' )
        {
          break;
        }
      }
      if(crlfPos >= trfLen - 2) //Try to read more
      {
        if( trfLen < CHUNK_SIZE )
        {
          size_t newTrfLen;
          ret = recv(buf + trfLen, 0, CHUNK_SIZE - trfLen - 1, &newTrfLen);
          trfLen += newTrfLen;
          if(ret != OK) goto connerr;
          continue;
        }
        else
        {
          goto prtclerr;
        }
      }
      buf[crlfPos] = '\0';
      int n = sscanf(buf, "%x", &readLen);
      if(n!=1)
      {
        ERR("Could not read chunk length");
        goto prtclerr;
      }

      memmove(buf, &buf[crlfPos+2], trfLen - (crlfPos + 2));
      trfLen -= (crlfPos + 2);

      if( readLen == 0 )
      {
        //Last chunk
        break;
      }
    }
    else
    {
      readLen = recvContentLength;
    }

    DBG("Retrieving %d bytes", readLen);

    do
    {
      pDataIn->write(buf, MIN(trfLen, readLen));
      if( trfLen > readLen )
      {
        memmove(buf, &buf[readLen], trfLen - readLen);
        trfLen -= readLen;
        readLen = 0;
      }
      else
      {
        readLen -= trfLen;
      }

      if(readLen)
      {
        ret = recv(buf, 1, CHUNK_SIZE - trfLen - 1, &trfLen);
        if(ret != OK) goto connerr;

      }
    } while(readLen);

    if( recvChunked )
    {
      if(trfLen < 2)
      {
        size_t newTrfLen;
        //Read missing chars to find end of chunk
        ret = recv(buf, 2 - trfLen, CHUNK_SIZE, &newTrfLen);
        if(ret != OK) goto connerr;
        trfLen += newTrfLen;
      }
      if( (buf[0] != '\r') || (buf[1] != '\n') )
      {
        ERR("Format error");
        goto prtclerr;
      }
      memmove(buf, &buf[2], trfLen - 2);
      trfLen -= 2;
    }
    else
    {
      break;
    }

  }

  socket::close(m_sock);
  DBG("Completed HTTP transaction");

  return OK;

  connerr:
    socket::close(m_sock);
    ERR("Connection error (%d)", ret);
  return NET_CONN;

  prtclerr:
    socket::close(m_sock);
    ERR("Protocol error");
  return NET_PROTOCOL;

}

int HTTPClient::recv(char* buf, size_t minLen, size_t maxLen, size_t* pReadLen) //0 on success, err code on failure
{
  DBG("Trying to read between %d and %d bytes", minLen, maxLen);
  size_t readLen = 0;
  while(readLen < minLen)
  {
    //Wait for socket to be readable
    //Creating FS set
    fd_set socksSet;
    FD_ZERO(&socksSet);
    FD_SET(m_sock, &socksSet);
    struct timeval t_val;
    t_val.tv_sec = m_timeout / 1000;
    t_val.tv_usec = (m_timeout - (t_val.tv_sec * 1000)) * 1000;
    int ret = socket::select(FD_SETSIZE, &socksSet, NULL, NULL, &t_val);
    if(ret <= 0 || !FD_ISSET(m_sock, &socksSet))
    {
      WARN("Timeout");
      return NET_TIMEOUT; //Timeout
    }

    ret = socket::recv(m_sock, buf + readLen, maxLen - readLen, 0);
    if( ret > 0)
    {
      readLen += ret;
      continue;
    }
    else if( ret == 0 )
    {
      WARN("Connection was closed by server");
      return NET_CLOSED; //Connection was closed by server
    }
    else
    {
      ERR("Connection error (recv returned %d)", ret);
      return NET_CONN;
    }
  }
  *pReadLen = readLen;
  DBG("Read %d bytes", readLen);
  return OK;
}

int HTTPClient::send(char* buf, size_t len) //0 on success, err code on failure
{
  if(len == 0)
  {
    len = strlen(buf);
  }
  DBG("Trying to write %d bytes", len);
  size_t writtenLen = 0;
  while(writtenLen < len)
  {
    //Wait for socket to be writeable
    //Creating FS set
    fd_set socksSet;
    FD_ZERO(&socksSet);
    FD_SET(m_sock, &socksSet);
    struct timeval t_val;
    t_val.tv_sec = m_timeout / 1000;
    t_val.tv_usec = (m_timeout - (t_val.tv_sec * 1000)) * 1000;
    int ret = socket::select(FD_SETSIZE, NULL, &socksSet, NULL, &t_val);
    if(ret <= 0 || !FD_ISSET(m_sock, &socksSet))
    {
      WARN("Timeout");
      return NET_TIMEOUT; //Timeout
    }

    ret = socket::send(m_sock, buf + writtenLen, len - writtenLen, 0);
    if( ret > 0)
    {
      writtenLen += ret;
      continue;
    }
    else if( ret == 0 )
    {
      WARN("Connection was closed by server");
      return NET_CLOSED; //Connection was closed by server
    }
    else
    {
      ERR("Connection error (recv returned %d)", ret);
      return NET_CONN;
    }
  }
  DBG("Written %d bytes", writtenLen);
  return OK;
}

int HTTPClient::parseURL(const char* url, char* scheme, size_t maxSchemeLen, char* host, size_t maxHostLen, uint16_t* port, char* path, size_t maxPathLen) //Parse URL
{
  char* schemePtr = (char*) url;
  char* hostPtr = (char*) strstr(url, "://");
  if(hostPtr == NULL)
  {
    WARN("Could not find host");
    return NET_INVALID; //URL is invalid
  }

  if( maxSchemeLen < hostPtr - schemePtr + 1 ) //including NULL-terminating char
  {
    WARN("Scheme str is too small (%d >= %d)", maxSchemeLen, hostPtr - schemePtr + 1);
    return NET_TOOSMALL;
  }
  memcpy(scheme, schemePtr, hostPtr - schemePtr);
  scheme[hostPtr - schemePtr] = '\0';

  hostPtr+=3;

  size_t hostLen = 0;

  char* portPtr = strchr(hostPtr, ':');
  if( portPtr != NULL )
  {
    hostLen = portPtr - hostPtr;
    portPtr++;
    if( sscanf(portPtr, "%d", &port) != 1)
    {
      WARN("Could not find port");
      return NET_INVALID;
    }
  }
  else
  {
    *port=0;
  }
  char* pathPtr = strchr(hostPtr, '/');
  if( hostLen == 0 )
  {
    hostLen = pathPtr - hostPtr;
  }

  if( maxHostLen < hostLen + 1 ) //including NULL-terminating char
  {
    WARN("Host str is too small (%d >= %d)", maxHostLen, hostLen + 1);
    return NET_TOOSMALL;
  }
  memcpy(host, hostPtr, hostLen);
  host[hostLen] = '\0';

  size_t pathLen;
  char* fragmentPtr = strchr(hostPtr, '#');
  if(fragmentPtr != NULL)
  {
    pathLen = fragmentPtr - pathPtr;
  }
  else
  {
    pathLen = strlen(pathPtr);
  }

  if( maxPathLen < pathLen + 1 ) //including NULL-terminating char
  {
    WARN("Path str is too small (%d >= %d)", maxPathLen, pathLen + 1);
    return NET_TOOSMALL;
  }
  memcpy(path, pathPtr, pathLen);
  path[pathLen] = '\0';

  return OK;
}

