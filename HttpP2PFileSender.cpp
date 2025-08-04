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

template <char delimiter>
static auto split(const std::string_view& str) {
	std::vector<std::string_view> result;
	size_t start = 0;
	while (start <= str.size()) {
		size_t end = str.find(delimiter, start);
		if (end == std::string_view::npos) {
			result.emplace_back(str.substr(start));
			break;
		}
		result.emplace_back(str.substr(start, end - start));
		start = end + 1;
	}
	return result;
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

	void sendHttpFile(const HANDLE& fileHandle, size_t size)
	{
		size_t sent = 0;

		do {
			DWORD bytes = std::min(size - sent, static_cast<size_t>(TRANSMITFILE_MAX));

			auto timeStart = std::chrono::system_clock::now();
			if (TransmitFile(socket, fileHandle, bytes, 0, NULL, NULL, 0) == FALSE) {
				throw std::system_error(WSAGetLastError(), std::system_category(), "Error sending file");
			}
			auto timeEnd = std::chrono::system_clock::now();

			double seconds = std::chrono::duration<double>(timeEnd - timeStart).count();

			sent += bytes;

			std::cerr << "Progress: " << (((double)sent) / size) * 100 << " %"". ""Average transmission speed: " << (bytes/seconds)/1000000 <<" MB/s." << std::endl;
		} while (sent < size);
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

	std::string formatTime(const std::chrono::utc_clock::time_point& time)
	{
		auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
		return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", seconds);
	}

	void sendTime()
	{
		sendString("Date: ");
		sendString(formatTime(std::chrono::utc_clock::now()));
		sendString("\r\n");
	}

private:
	void ignoreAllInput()
	{
		char buf[4096];
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

			std::cerr << "Method not allowed. Ignoring." << std::endl;
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
					std::cerr << "Filename weird" << std::endl;
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

				std::cerr << "Requested file not the same as hosted: " << word << std::endl;
				//std::cerr << "Hosted:" << filename.filename().string() << std::endl;

				return;
			}
		}
		LONGLONG filesize = std::filesystem::file_size(filename);

		std::vector<std::pair<size_t, size_t>> ranges;


		while (true)
		{
			std::string line = connection.getLine();

			// Parse range
			if (line.starts_with("Range: bytes="))
			{
				std::string_view rangeText(line);
				rangeText = rangeText.substr(13);
				auto rangesText = split<','>(rangeText);

				ranges.reserve(rangesText.size());

				for (const auto& rangeText : rangesText)
				{
					size_t start, end;

					auto pos = rangeText.find_first_of('-');
					start = std::atoll(rangeText.data());

					if (pos + 1 >= rangeText.size())
						end = filesize;
					else
						end = std::atoll(rangeText.data() + pos + 1);

					ranges.emplace_back(start, end);
				}
			}

			// TODO maybe add other headers that are relevant here

			// Ignore everything else and stop after all is received
			else if (line.empty())
				break;
		}


		if (ranges.size() > 1)
		{
			std::cerr << "Client requests multipart ranges, this is not yet supported. Sending the full file" << std::endl;
			ranges.clear();
		}

		bool partialContent = !ranges.empty();

		if (partialContent)
		{
			// Actual correct response
			connection.sendString(
				"HTTP/1.0 206 Partial Content\r\n"
			);
			std::cerr << "Sending a part of the file (resuming download at " << (((double)ranges[0].first) / filesize)*100 << " %" << ")..." << std::endl;
		}
		else
		{
			// Actual correct response
			connection.sendString(
				"HTTP/1.0 200 OK\r\n"
			);
			std::cerr << "Sending the whole file..." << std::endl;
		}

		connection.sendTime();

		
		wil::unique_handle fileHandle(CreateFileW(filename.native().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL));

		if (!fileHandle)// [[unlikely]]
			throw std::system_error(GetLastError(), std::system_category(), "Error opening file");

		if (ranges.empty())
			ranges.emplace_back(0, filesize);


		connection.sendString(
			"Connection: close\r\n"
			"Cache-Control: no-store, no-transform\r\n"
			"Accept-Ranges: bytes\r\n"
		);

		connection.sendString("Last-Modified: ");
		connection.sendString(connection.formatTime(std::chrono::utc_clock::from_sys(std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filename)))));
		connection.sendString("\r\n");


		// TODO change to support multipart ranges
		connection.sendString("Content-Length: ");
		connection.sendString(std::to_string(ranges[0].second - ranges[0].first));
		connection.sendString("\r\n");

		std::string type;
		{
			LPWSTR pwzMimeOut = NULL;
			HRESULT hr = FindMimeFromData(NULL, filename.native().c_str(), NULL, 0, NULL, FMFD_URLASFILENAME, &pwzMimeOut, 0);
			
			if (hr == S_OK) {
				type = ConvertLPCWSTRToString(pwzMimeOut);
			}
		}

		for (const auto& range : ranges)
		{
			connection.sendString("Content-Type: ");
			connection.sendString(type);
			connection.sendString("\r\n");

			if (partialContent)
			{
				connection.sendString("Content-Range: bytes ");
				connection.sendString(std::to_string(range.first));
				connection.sendString("-");
				connection.sendString(std::to_string(range.second));
				connection.sendString("/");
				connection.sendString(std::to_string(filesize));
				connection.sendString("\r\n");
			}

			connection.sendString("\r\n");

			// Set file pointer to requested position
			LARGE_INTEGER start{ .QuadPart = (long long)range.first };
			SetFilePointerEx(fileHandle.get(), start, NULL, FILE_BEGIN);

			// Send the requested amount of bytes
			connection.sendHttpFile(fileHandle.get(), range.second - range.first);
		}

		std::cerr << "File sent successfully" << std::endl;
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
        std::cerr << "Missing file path argument" << std::endl;
        return EXIT_FAILURE;
    }

	filename = argv[1];



	if (!std::filesystem::is_regular_file(filename)) [[unlikely]]
	{
		std::cerr << "File does not exist" << std::endl;
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
				std::cerr << "Filename too long" << std::endl;
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
		std::cerr << e.what() << std::endl;
	}

}
