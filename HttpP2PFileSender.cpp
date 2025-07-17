#define NOMINMAX
#include <iostream>

//#include <windows.h>
//#include <ole2.h>

#include <array>
#include <sstream>
#include <thread>
#include <fstream>
#include <filesystem>
#include <Winsock2.h>
#include <MSWSock.h>
#include <urlmon.h>
#include <atlstr.h>
#include <wil/resource.h>

#include "upnpPortOpener.h"

#pragma comment( lib, "urlmon" )

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")


constexpr LONGLONG TRANSMITFILE_MAX = (2ull << 30) - 1;

std::filesystem::path filename;


// Function to convert LPCWSTR to std::string 
std::string ConvertLPCWSTRToString(LPCWSTR lpcwszStr)
{
	// Determine the length of the converted string 
	int strLength = WideCharToMultiByte(CP_UTF8, 0, lpcwszStr, -1, nullptr, 0, nullptr, nullptr);

	// Create a std::string with the determined length 
	std::string str(strLength, 0);

	// Perform the conversion from LPCWSTR to std::string 
	WideCharToMultiByte(CP_UTF8, 0, lpcwszStr, -1, &str[0], strLength, nullptr, nullptr);

	// Return the converted std::string 
	return std::string(str.c_str());
}

class TcpConnection {
	SOCKET socket;

public:
	/// <summary>
	/// Encapsulates an active TCP connection
	/// </summary>
	/// <param name="socket">Already accepted socket</param>
	TcpConnection(SOCKET&& socket) : socket(std::move(socket)) {}

	~TcpConnection()
	{
		shutdown(socket, SD_SEND);

		ignoreAllInput();

		closesocket(socket);
	}


	/// <summary>
	/// Send string response to the client
	/// </summary>
	void sendString(const std::string_view& string)
	{
		send(socket, string.data(), static_cast<int>(string.size()), 0);
	}

	void sendHttpFile(const std::filesystem::path& filename)
	{
		LONGLONG filesize = std::filesystem::file_size(filename);
		wil::unique_handle fileHandle(CreateFileW(filename.native().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL));

		if (!fileHandle) [[unlikely]]
			throw std::system_error(GetLastError(), std::system_category(), "Error opening file");


		sendString(
			"Connection: close\r\n"
			"Cache-Control: no-store, no-transform\r\n"
			"Content-Length: "
		);

		sendString(std::to_string(filesize));

		{
			LPWSTR pwzMimeOut = NULL;
			HRESULT hr = FindMimeFromData(NULL, filename.native().c_str(), NULL, 0, NULL, FMFD_URLASFILENAME, &pwzMimeOut, 0);
			if (hr == S_OK) {
				sendString(
					"\r\n"
					"Content-Type: "
				);
				sendString(ConvertLPCWSTRToString(pwzMimeOut));
			}
		}

		sendString(
			"\r\n"
			"\r\n"
		);

		LARGE_INTEGER total_bytes{ 0 };
		total_bytes.QuadPart = 0;

		do {
			auto bytes = std::min(filesize - total_bytes.QuadPart, TRANSMITFILE_MAX);
			if (TransmitFile(socket, fileHandle.get(), bytes, 0, NULL, NULL, 0) == FALSE) {
				throw std::system_error(WSAGetLastError(), std::system_category(), "Error sending file");
			}

			total_bytes.QuadPart += bytes;
			SetFilePointerEx(fileHandle.get(), total_bytes, NULL, FILE_BEGIN);
		} while (total_bytes.QuadPart < filesize);
	}

	std::string getLine()
	{
		std::stringstream ss;

		char tmp;

		while (recv(socket, &tmp, 1, 0) > 0)
		{
			if (tmp == '\r')
			{
				recv(socket, &tmp, 1, 0);
				if (tmp == '\n')
					return ss.str();
				else
					ss << '\r' << tmp;
			}
			ss << tmp;
		}
		
		throw std::system_error(WSAGetLastError(), std::system_category(), "Error receiving bytes");
	}

	void sendTime()
	{
		char buf[128];
		time_t now = time(0);
		struct tm tm;
		gmtime_s(&tm, &now);
		auto res = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S", &tm);

		if (res != 0) [[likely]]
			{
				sendString("Date: ");
				sendString(buf);
				sendString(" GMT\r\n");
			}
	}

private:
	void ignoreAllInput()
	{
		char buf[1024];
		while (true) {
			auto numRead = recv(socket, buf, sizeof(buf), 0);
			if (numRead == SOCKET_ERROR) [[unlikely]]
				break;//throw std::system_error(WSAGetLastError(), std::system_category(), "Error receiving bytes");
		    if (numRead == 0)
				break;
		}
	}

	void ignoreBytes(size_t numBytes)
	{
		char buf[256];
		while (numBytes > 0)
		{
			int numRead = recv(socket, (char*)buf, std::min(sizeof(buf), numBytes), 0);
			if (numRead == SOCKET_ERROR) [[unlikely]]
				throw std::system_error(WSAGetLastError(), std::system_category(), "Error receiving bytes");
			numBytes -= numRead;
		}
	}

	void sendData(const char* data, int len)
	{
		int sent = 0;

		do
		{
			auto res = send(socket, data + sent, len - sent, 0);
			if (res == SOCKET_ERROR) [[unlikely]]
				throw std::system_error(WSAGetLastError(), std::system_category(), "Error sending bytes to buffer");
			sent += res;
		} while (sent < len);
	}



};


/// <summary>
/// Block on one connection and serve the client
/// </summary>
/// <param name="s">Already accepted socket</param>
void serveConnection(SOCKET&& s)
{
	try
	{
		TcpConnection connection(std::move(s));

		std::stringstream line;
		line << connection.getLine();

		std::string word;

		line >> word;

		if (word != "GET")
		{
			connection.sendString(
				"HTTP/1.0 405 Method Not Allowed\r\n"
			);
			connection.sendTime();
			connection.sendString(
				"Connection: close\r\n"
				"Content-Length: 0\r\n"
				"\r\n"
			);

			std::cout << "Method not allowed. Ignoring." << std::endl;
			return;
		}
		{
			// Ignore the first / character
			char tmp;
			line >> tmp;
		}
		line >> word;

		{
			std::wstring fileUnescaped(word.begin(), word.end());

			{
				DWORD fileNameSize = fileUnescaped.size();
				if (UrlUnescapeW(&fileUnescaped[0], NULL, &fileNameSize, URL_UNESCAPE_INPLACE | URL_UNESCAPE_AS_UTF8) != S_OK) [[unlikely]]
					{
						std::cout << "Filename weird" << std::endl;
						return;
					}
			}

			if (wcscmp(fileUnescaped.c_str(), filename.filename().c_str()) != 0)
			{
				connection.sendString(
					"HTTP/1.0 404 Not Found\r\n"
				);
				connection.sendTime();
				connection.sendString(
					"Connection: close\r\n"
					"Content-Length: 0\r\n"
					"\r\n"
				);

				std::cout << "Requested file not the same as hosted: " << word << std::endl;
				//std::cerr << "Hosted:" << filename.filename().string() << std::endl;

				return;
			}
		}

		// Accept the whole request
		while (!connection.getLine().empty()) {}

		std::cout << "Sending the file..." << std::endl;

		// Actual correct response
		connection.sendString(
			"HTTP/1.0 200 OK\r\n"
		);

		connection.sendTime();

		connection.sendHttpFile(filename);

		std::cout << "File sent successfully" << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Connection error: " << e.what() << std::endl;
	}
}

class TcpServer {
	WSADATA wsa;
	SOCKET bindS;

	SOCKET bindSocket(uint16_t port)
	{
		sockaddr_in server;
		server.sin_family = AF_INET;
		server.sin_addr.S_un.S_addr = 0;
		server.sin_port = htons(port);

		SOCKET s = socket(AF_INET, SOCK_STREAM, NULL);

		if (s == SOCKET_ERROR)
			throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot bind to this port");

		int iResult;
		iResult = bind(s, (struct sockaddr*)&server, sizeof(server)); // binding the Host Address and Port Number
		if (iResult == SOCKET_ERROR)
			throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot start server");

		iResult = listen(s, AF_INET);

		return s;
	}
public:
	TcpServer(uint16_t port)
	{
		//std::cerr << "Starting server at port " << port << std::endl;

		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot start server");

		bindS = bindSocket(port);
	}

	~TcpServer()
	{
		closesocket(bindS);
		WSACleanup();
	}

	void serve()
	{
		while (true)
		{
			try
			{
				SOCKET res = accept(bindS, NULL, NULL); // Accept a connection on a new Socket made for the Client.
				if (res == SOCKET_ERROR)
					throw std::system_error(WSAGetLastError(), std::system_category(), "Error accepting a connection");

				//std::cerr << "Server accepted a new connection." << std::endl;
				std::thread thread(serveConnection, std::move(res));
				thread.detach();
			}
			catch (const std::exception& ex)
			{
				std::cerr << "Server error: " << ex.what() << std::endl;
			}
		}
	}
};



int wmain(int argc, wchar_t* argv[])
{
	// Set console locale
	//std::setlocale(LC_ALL, "");
	//std::locale::global(std::locale(""));
	//std::cout.imbue(std::locale());

    if (argc < 2) [[unlikely]]
    {
        std::cout << "Missing file path argument" << std::endl;
        return EXIT_FAILURE;
    }

	filename = argv[1];



	if (!std::filesystem::is_regular_file(filename)) [[unlikely]]
	{
		std::cout << "File does not exist" << std::endl;
		return EXIT_FAILURE;
	}


	try
	{
		//int port = 8080;
		//auto openedPort = openRandomDynamicPort();
		auto portNumber = 15935;// openedPort.getExternalPort();

		TcpServer server(portNumber);

		//  

		//std::cout << openedPort.getExternalIPAddress().GetAddress();

		{
			constexpr DWORD bufferSize = 1024;
			wchar_t filenameUrl[bufferSize];
			DWORD bufferUsed = bufferSize;

			auto returnCode = UrlEscapeW(filename.filename().native().c_str(), filenameUrl, &bufferUsed, URL_ESCAPE_ASCII_URI_COMPONENT | URL_ESCAPE_AS_UTF8);

			if (returnCode == E_POINTER)
			{
				std::cout << "Filename too long" << std::endl;
				return EXIT_FAILURE;
			}
			else if (returnCode != S_OK)
				return EXIT_FAILURE;

			std::wcout
				//<< '\n'
				<< "http://"
				<< "IP"
				//<< (LPCSTR)openedPort.getExternalIPAddress()
				<< ":"
				<< portNumber
				<< '/'
				<< filenameUrl
				<< std::endl;
		}

		server.serve();
	}
	catch (const std::exception& e)
	{
		std::cout << e.what() << std::endl;
	}

}
