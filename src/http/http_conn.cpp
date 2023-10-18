#include "http_conn.h"

/*定义HTTP响应的一些状态信息*/
const char* ok_200_title="OK";
const char* error_400_title="Bad Request";
const char* error_400_form="Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title="Forbidden";
const char* error_403_form="You do not have permission to get file from this server.\n";
const char* error_404_title="Not Found"; 
const char* error_404_form="The requested file was not found on this server.\n";
const char* error_500_title="Internal Error";
const char* error_500_form="There was an unusual problem serving the requested file.\n";
/*网站的根目录*/
const char* doc_root="/var/www/html";

int setnonblocking(int fd){
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    if(one_shot){
        event.events|=EPOLLONESHOT;
    }  
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}
int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;

void http_conn::close_conn(bool real_close){
    if(real_close&&(m_sockfd!=-1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;/*关闭一个连接时，将客户总量减1*/
    }
}
void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd=sockfd;
    m_addr=addr;
    /*如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉*/
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    _init();
}
void http_conn::_init(){
    m_check_state=STATE_REQUESTLINE;
    m_linger=false;
    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length=0;
    m_host=0;
    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;
    m_write_idx=0;
    memset(m_read_buf,'\0',READ_BUF_SIZE);
    memset(m_write_buf,'\0',WRITE_BUF_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}
/*从状态机，其分析请参考8.6节，这里不再赘述*/
http_conn::LINE_STATUS http_conn::_ParseLine(){
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r'){
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx+1]=='\n'){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(temp=='\n'){
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r')){
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}
/*循环读取客户数据，直到无数据可读或者对方关闭连接*/
bool http_conn::read(){
    if(m_read_idx>=READ_BUF_SIZE){
        return false;
    }
    int bytes_read=0;
    while(true){
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUF_SIZE-m_read_idx,0);
        if(bytes_read==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            return false;
        }else if(bytes_read==0){
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}
/*解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号*/
http_conn::HTTP_CODE http_conn::_ParseRequestLine(char*text){
    m_url=strpbrk(text,"\t");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++='\0';
    char* method=text;
    if(strcasecmp(method,"GET")==0){
        m_method=GET;
    }else{
        return BAD_REQUEST;
    }
    m_url+=strspn(m_url,"\t");
    m_version=strpbrk(m_url,"\t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version,"\t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7)==0){
        m_url+=7;
        m_url=strchr(m_url,'/');
    }
    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }
    m_check_state= STATE_HEADER;
    return NO_REQUEST;
}
/*解析HTTP请求的一个头部信息*/
http_conn::HTTP_CODE http_conn::_ParseHeaders(char*text){
    /*遇到空行，表示头部字段解析完毕*/
    if(text[0]=='\0'){
    /*如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机
    转移到CHECK_STATE_CONTENT状态*/
        if(m_content_length!=0){
            m_check_state=STATE_CONTENT;
            return NO_REQUEST;
        }
    /*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }else if(strncasecmp(text,"Connection:",11)==0) {/*处理Connection头部字段*/
        text+=11;
        text+=strspn(text,"\t");
        if(strcasecmp(text,"keep-alive")==0){
            m_linger=true;
        }
    }else if(strncasecmp(text,"Content-Length:",15)==0){/*处理Content-Length头部字段*/
        text+=15;
        text+=strspn(text,"\t");
        m_content_length=atoi(text);
    }else if(strncasecmp(text,"Host:",5)==0){/*处理Host头部字段*/
        text+=5;
        text+=strspn(text,"\t");
        m_host=text;
    }else{
        std::cout<<"oop!unknow header "<<text<<std::endl;
    }
    return NO_REQUEST;
}
/*我们没有真正解析HTTP请求的消息体，只是判断它是否被完整地读入了*/
http_conn::HTTP_CODE http_conn::_ParseContent(char*text){
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
/*主状态机。其分析请参考8.6节，这里不再赘述*/
http_conn::HTTP_CODE http_conn::_process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char*text=0;
    while(((m_check_state==STATE_CONTENT)&&(line_status==LINE_OK))||((line_status=_ParseLine())==LINE_OK)){
        text=_GetLine();
        m_start_line=m_checked_idx;
        printf("got 1 http line:%s\n",text);
        switch(m_check_state){
            case STATE_REQUESTLINE:{
                ret=_ParseRequestLine(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case STATE_HEADER:{
                ret=_ParseHeaders(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret==GET_REQUEST){
                    return _DoRequest();
                }
                break;
            }
            case STATE_CONTENT:{
                ret=_ParseContent(text);
                if(ret==GET_REQUEST){
                    return _DoRequest();
                }
                line_status=LINE_OPEN;
                break;
            }default:{
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}
/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存
在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address
处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::_DoRequest(){
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat)<0){
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode&S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }
    int fd=open(m_real_file,O_RDONLY);
    m_file_addr= (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}
/*对内存映射区执行munmap操作*/
void http_conn::_Unmap()
{
    if(m_file_addr){
        munmap(m_file_addr,m_file_stat.st_size);
        m_file_addr=0;
    }
}
/*写HTTP响应*/
bool http_conn::write(){
    int temp=0;
    int bytes_have_send=0;
    int bytes_to_send=m_write_idx;
    if(bytes_to_send==0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        _init();
        return true;
    }
    while(1){
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if(temp<=-1){
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无
            法立即接收到同一客户的下一个请求，但这可以保证连接的完整性*/
            if(errno==EAGAIN){
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            _Unmap();
            return false;
        }
        bytes_to_send-=temp;
        bytes_have_send+=temp;
        if(bytes_to_send<=bytes_have_send){
            /*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            _Unmap();
            if(m_linger){
                _init();
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }else{
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return false;
            }
        }
    }
}
/*往写缓冲中写入待发送的数据*/
bool http_conn::_AddResponse(const char*format,...){
    if(m_write_idx>=WRITE_BUF_SIZE){
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUF_SIZE-1-m_write_idx,format,arg_list);
    if(len>=(WRITE_BUF_SIZE-1-m_write_idx)){
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

bool http_conn::_AddStatusLine(int status,const char*title){
    return _AddResponse("%s%d%s\r\n","HTTP/1.1",status,title);
}
bool http_conn::_AddHeaders(int content_len)
{
    _AddContentLength(content_len);
    _AddLinger();
    _AddBlankLine();
}
bool http_conn::_AddContentLength(int content_len){
    return _AddResponse("Content-Length:%d\r\n",content_len);
}
bool http_conn::_AddLinger(){
    return _AddResponse("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}
bool http_conn::_AddBlankLine(){
    return _AddResponse("%s","\r\n");
}
bool http_conn::_AddContent(const char*content){
    return _AddResponse("%s",content);
}

/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::_process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:{
            _AddStatusLine(500,error_500_title);
            _AddHeaders(strlen(error_500_form));
            if(!_AddContent(error_500_form)){
                return false;
            }
            break;
        }
        case BAD_REQUEST:{
            _AddStatusLine(400,error_400_title);
            _AddHeaders(strlen(error_400_form));
            if(!_AddContent(error_400_form)){
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            _AddStatusLine(404,error_404_title);
            _AddHeaders(strlen(error_404_form));
            if(!_AddContent(error_404_form)){
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{
            _AddStatusLine(403,error_403_title);
            _AddHeaders(strlen(error_403_form));
            if(!_AddContent(error_403_form)){
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            _AddStatusLine(200,ok_200_title);
            if(m_file_stat.st_size!=0){
                _AddHeaders(m_file_stat.st_size);
                m_iv[0].iov_base=m_write_buf;
                m_iv[0].iov_len=m_write_idx;
                m_iv[1].iov_base=m_file_addr;
                m_iv[1].iov_len=m_file_stat.st_size;
                m_iv_count=2;
                return true;
            }else{
                const char*ok_string="<html><body></body></html>";
                _AddHeaders(strlen(ok_string));
                if(!_AddContent(ok_string)){
                    return false;
                }
            }
        }
        default:{
            return false;
        }
    }
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;
}
/*由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process(){
    HTTP_CODE read_ret=_process_read();
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }
    bool write_ret=_process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}