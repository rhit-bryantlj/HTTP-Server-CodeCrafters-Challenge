#include <iostream>
#include <cstdlib>
#include <string>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <filesystem>
#include <vector>
#include <format>
#include <fstream>
#include <algorithm>
#include <zlib.h>
#include <thread>

#define MAX_RECV_LEN 8000

std::vector<std::string> encodings = {"gzip"};

std::string makeResponse(bool encode, std::string encodeType, std::string contentType, int size, std::string content){
  std::string response;
  if(encode){
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw(std::runtime_error("deflateInit failed while compressing."));
    zs.next_in = (Bytef*)content.data();
    zs.avail_in = content.size();
    int ret;
    char outbuffer[32768];
    std::string outstring;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);
        ret = deflate(&zs, Z_FINISH);
        if (outstring.size() < zs.total_out) {
            outstring.append(outbuffer, zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        throw(std::runtime_error("Exception during zlib compression: " + std::to_string(ret)));
    }
    response = std::format("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Encoding: {}\r\nContent-Length: {}\r\n\r\n{}",contentType, encodeType, outstring.length(), outstring);
  }else{
    response = std::format("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n\r\n{}",contentType, size, content);
  }

  return response;
}

void handle_client(int client_fd, std::string dir){
  char buf[MAX_RECV_LEN];
  int bytesReceived = 0;
  bool persist = true;
  std::string response;

  while(1){
    bytesReceived = recv(client_fd, buf, sizeof(buf), 0);
    if(bytesReceived == 0){
      // should never get here with the test for now but later will maybe come up
      std::cout << "Nothing received\n";
      break;
    }
    std::string input(buf);
    int space_pos = input.find(" ");
    std::string url = "";
    for(int i = space_pos + 1; i < bytesReceived; i++){
      if(buf[i] == ' ')
        break;
      url += buf[i];
    }
    size_t encodingPos = input.find("Accept-Encoding");
    std::vector<std::string> inputEncodings = {};
    if(encodingPos != std::string::npos){
      std::string curEncoding = "";
      for(int i = (int)encodingPos + 17; i < input.length(); i++){
        if(input[i] == '\r'){
          inputEncodings.push_back(curEncoding);
          break;
        }
        if(input[i] == ','){
          inputEncodings.push_back(curEncoding);
          curEncoding = "";
          i++;
        }else {
          curEncoding+=input[i];
        }
      }
    }
    bool encodeResponse = false;
    std::string encodingType = "";
    for(int i = 0; i < inputEncodings.size(); i++){
      if(std::find(encodings.begin(), encodings.end(), inputEncodings[i]) != encodings.end()){ // just exit on the first working encoding for now
        encodeResponse = true;
        encodingType = inputEncodings[i];
        break;
      }
    }
    // test for connection close header
    if(input.find("Connection: close")!= std::string::npos){
      persist = false;
    }
      
    if(url == "/"){
      response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
    }else if(url.substr(0,6) == "/files"){
      std::string filepath = dir + url.substr(7);
      std::ifstream file(filepath);
      std::cout << "filepath: " + filepath << " type: " + input.substr(0,4) << "\n";
      if(input.substr(0,4) == "POST"){
        int pos = input.find("Content-Length:");
        std::string temp = input.substr(pos+16);
        pos = temp.find("\r");
        int content_len = std::stoi(temp.substr(0,pos));
        std::string content = temp.substr(temp.length()-content_len);
        std::cout << content << std::endl;
        if(std::filesystem::exists(dir)){
          std::ofstream file;
          file.open(filepath);
          file.write(content.c_str(), content_len);
          file.close();
          response = "HTTP/1.1 201 Created\r\n\r\n";
        } else {
          response = "HTTP/1.1 404 Not Found\r\n\r\n";
        }
      } else{
        if(file.good()){
          std::stringstream content;
          content << file.rdbuf();
          response = makeResponse(encodeResponse, encodingType, "application/octet-stream", content.str().size(), content.str());
        } else{
          response = "HTTP/1.1 404 Not Found\r\n\r\n";
        }
      }
    }else if(url.substr(0, 5) == "/echo"){
      response = makeResponse(encodeResponse, encodingType, "text/plain", url.substr(6).size(), url.substr(6));
    } else if (url == "/user-agent"){
      int pos = input.find("User-Agent: ");
      std::string content = input.substr(pos+12);
      std::cout << content;
      response = makeResponse(encodeResponse, encodingType, "text/plain", content.length()-4, content);
    } else{
      std::filesystem::path url_path(url);
      if(std::filesystem::is_directory(url_path)){
        response = "HTTP/1.1 200 OK\r\n\r\n";
      } else{
        response = "HTTP/1.1 404 Not Found\r\n\r\n";
      }
    }

    // response message created, either send and close, or just send and receive again
    if(persist == false){
      response.insert(response.find("\r\n")+2, "Connection: close\r\n");
      send(client_fd, response.c_str(), response.length(), 0);
      break;
    } else{
      send(client_fd, response.c_str(), response.length(), 0);
    }
  }
  close(client_fd);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string dir;
  if (argc == 3 && strcmp(argv[1], "--directory") == 0)
  	dir = argv[2];

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(4221);
  
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }
  
  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }
  
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  while(1){
    std::cout << "Waiting for a client to connect...\n";
  
    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);

    std::cout << "Client connected\n";

    std::thread t(handle_client, client_fd, dir);
    t.detach();
  }
  
  close(server_fd);

  return 0;
}
