#include "common.h"


struct proxy_node *proxy_node_new(char *proxy_string, int proxy_type){

	struct proxy_node *new_node;

	char *first = NULL;
	char *second = NULL;
	char *third = NULL;
	char *fourth = NULL;

	unsigned int count;
	char *tmp;


	// First, quick syntax check.
	count = 0;
	tmp = proxy_string;
	while((tmp = strchr(tmp, ':')) != NULL){
		count++;
		tmp++;
	}

	if((proxy_type == PROXY_DYNAMIC && !(count == 0 || count == 1)) \
			|| (proxy_type == PROXY_STATIC && !(count == 2 || count == 3))){
		report_error("proxy_node_new(): Improper port forward syntax for proxy type '%d': %s", proxy_type, proxy_string);
		return(NULL);
	} 

	// Now let's start setting up the nodes.
	if((new_node = (struct proxy_node *) calloc(1, sizeof(struct proxy_node))) == NULL){
		report_error("proxy_node_new(): calloc(1, sizeof(struct proxy_node)): %s", strerror(errno));
		return(NULL);
	}
	new_node->type = proxy_type;	

	if((first = (char *) calloc(strlen(proxy_string) + 1, sizeof(char))) == NULL){
			report_error("proxy_node_new(): calloc(%d, sizeof(char)): %s", (int) strlen(proxy_string), strerror(errno));
		free(new_node);
		return(NULL);
	}

	strcpy(first, proxy_string);

	if((second = strchr(first, ':')) != NULL){
		*(second++) = '\0';
	}

	if(proxy_type == PROXY_DYNAMIC){

		if(second){
			new_node->lhost = first;
			new_node->lport = second;
		}else{
			new_node->lhost = DEFAULT_PROXY_ADDR;
			new_node->lport = first;
		}

	} else if(proxy_type == PROXY_STATIC) {

		if(!second || (third = strchr(second, ':')) == NULL){
			report_error("proxy_node_new(): Malformed proxy string: %s", proxy_string);
			goto CLEANUP;
		}

		if((fourth = strchr((third + 1) , ':')) != NULL){
			new_node->lhost = first;
			new_node->lport = second;
			*(third++) = '\0';
			new_node->rhost_rport = third;
		}else{
			new_node->lhost = DEFAULT_PROXY_ADDR;
			new_node->lport = first;
			new_node->rhost_rport = second;
		}

	} else {
		goto CLEANUP;
	}

	if(proxy_listen(new_node) == -1){
		report_error("proxy_node_new(): proxy_listen() failed. Skipping this proxy.");
		goto CLEANUP;
	}

	return(new_node);

CLEANUP:
	free(new_node);	
	free(first);
	return(NULL);
}

/* setup a new proxy listener */
int proxy_listen(struct proxy_node *cur_proxy_node){

	int yes = 1;
	int rv, listener;
	struct addrinfo hints, *ai, *p;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if((rv = getaddrinfo(cur_proxy_node->lhost, cur_proxy_node->lport, &hints, &ai)) != 0) {
		report_error("proxy_listen(): getaddrinfo(%s, %s, %lx, %lx): %s", \
				cur_proxy_node->lhost, cur_proxy_node->lport, (unsigned long) &hints, (unsigned long) &ai, gai_strerror(rv));
		return(-1);
	}

	for(p = ai; p != NULL; p = p->ai_next) {
		listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (listener < 0) { 
			continue;
		}

		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		if(bind(listener, p->ai_addr, p->ai_addrlen) < 0){
			close(listener);
			continue;
		}

		break;
	}

	if(p == NULL){
		report_error("proxy_listen(): Failed to bind() to %s:%s", cur_proxy_node->lhost, cur_proxy_node->lport);
		return(-1);
	}
	freeaddrinfo(ai); 

	if(listen(listener, 10) == -1) {
		report_error("proxy_listen(): listen(%d, 10): %s", listener, strerror(errno));					
		return(-1);
	}

	fcntl(listener, F_SETFL, O_NONBLOCK);
	cur_proxy_node->fd = listener;

	return(0);
}

int proxy_connect(char *rhost_rport){

	int count;
	int yes = 1;
	int rv, connector = -2;
	struct addrinfo hints, *ai, *p;
	char *rhost, *rport, *tmp_ptr;

	tmp_ptr = rhost_rport;
	if(*tmp_ptr == '['){
		tmp_ptr++;
	}

	count = strlen(tmp_ptr);

	if((rhost = (char *) calloc(count + 1, sizeof(char))) == NULL){
		report_error("proxy_connect(): calloc(%d, %d): %s", count + 1, (int) sizeof(char), strerror(errno));
		return(-1);
	}
	memcpy(rhost, tmp_ptr, count);

	if(tmp_ptr != rhost_rport){
		if((tmp_ptr = strchr(rhost, ']')) == NULL){
			free(rhost);
			return(-2);
		}
		*(tmp_ptr) = '\0';
	}

	if((rport = strrchr(rhost, ':')) == NULL){
		free(rhost);
		return(-2);
	}
	*(rport++) = '\0';

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if((rv = getaddrinfo(rhost, rport, &hints, &ai)) != 0) {
		report_error("proxy_connect(): getaddrinfo(%s, %s, %lx, %lx): %s", rhost, rport, (unsigned long) &hints, (unsigned long) &ai, gai_strerror(rv));
		free(rhost);
		return(-2);
	}

	for(p = ai; p != NULL; p = p->ai_next) {
		connector = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (connector < 0) { 
			continue;
		}

		setsockopt(connector, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		fcntl(connector, F_SETFL, O_NONBLOCK);
		errno = 0;
		if(connect(connector, p->ai_addr, p->ai_addrlen) < 0){
			if (errno != EINPROGRESS){
				close(connector);
				continue;
			}
		}
		break;
	}
	free(rhost);

	if(p == NULL){
		return(-2);
	}

	return(connector);
}

struct connection_node *connection_node_create(){

	struct connection_node *cur_connection_node, *tmp_connection_node;

	if((cur_connection_node = (struct connection_node *) calloc(1, sizeof(struct connection_node))) == NULL){
		report_error("connection_node_create(): calloc(1, %d): %s", (int) sizeof(struct connection_node), strerror(errno));
		return(NULL);
	}

	tmp_connection_node = io->connection_tail;
	if(!tmp_connection_node){
		io->connection_head = cur_connection_node;
		io->connection_tail = cur_connection_node;
	}else{
		tmp_connection_node->next = cur_connection_node;
		cur_connection_node->prev = tmp_connection_node;
		io->connection_tail = cur_connection_node;
	}

	io->fd_count++;
	return(cur_connection_node);
}

int connection_node_delete(struct connection_node *cur_connection_node){

	if(cur_connection_node == io->connection_head){
		io->connection_head = cur_connection_node->next;
	}
	if(cur_connection_node == io->connection_tail){
		io->connection_tail = cur_connection_node->prev;
	}
	if(cur_connection_node->prev){
		cur_connection_node->prev->next = cur_connection_node->next;
	}
	if(cur_connection_node->next){
		cur_connection_node->next->prev = cur_connection_node->prev;
	}

	if(cur_connection_node->fd){
		close(cur_connection_node->fd);
	}
	if(cur_connection_node->rhost_rport){
		free(cur_connection_node->rhost_rport);
	}
	if(cur_connection_node->buffer_head){
		free(cur_connection_node->buffer_head);
	}
	if(cur_connection_node){
		free(cur_connection_node);
	}

	io->fd_count--;
	return(0);
}


struct connection_node *connection_node_find(unsigned short origin, unsigned short id){
	struct connection_node *tmp_connection_node;

	tmp_connection_node = io->connection_head;
	while(tmp_connection_node){
		if((tmp_connection_node->origin == origin) && (tmp_connection_node->id == id)){
			return(tmp_connection_node);
		}
		tmp_connection_node = tmp_connection_node->next;
	}
	return(NULL);
}

void connection_node_queue(struct connection_node *cur_connection_node){

	if(cur_connection_node == io->connection_tail){
		return;
	}

	if(cur_connection_node == io->connection_head){
		io->connection_head = cur_connection_node->next;
	}

	if(cur_connection_node->prev){
		cur_connection_node->prev->next = cur_connection_node->next;
	}

	if(cur_connection_node->next){
		cur_connection_node->next->prev = cur_connection_node->prev;
	}

	io->connection_tail->next = cur_connection_node;
	cur_connection_node->prev = io->connection_tail;
	cur_connection_node->next = NULL;
	io->connection_tail = cur_connection_node;

}

int parse_socks_request(struct connection_node *cur_connection_node){

	int index, size;
	int nmethods, i;
	int len = 0;

	char *domain_name;
	char *head;
	char *dst_port_ptr = NULL;
	char *dst_addr_ptr = NULL;

	int atype = 0x01;
	int noauth_found;


	head = cur_connection_node->buffer_ptr;
	size = cur_connection_node->buffer_tail - cur_connection_node->buffer_ptr;
	index = 0;

	if(!size){
		return(CON_SOCKS_INIT);
	}

	cur_connection_node->ver = *(head);
	if(head[index] == 4){

		/*
			 +----+----+----+----+----+----+----+----+----+----+....+----+
			 | VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
			 +----+----+----+----+----+----+----+----+----+----+....+----+
			 1    1    2         4                   variable       1

			 1 + 1 + 2 + 4 + 1 
			 = 9  Minimum number of bytes to ensure we have at least 1 char of USERID.
		 */

		if(size < 9){
			return(CON_SOCKS_INIT);
		}

		index += 1;

		cur_connection_node->cmd = head[index];
		index += 1;

		dst_port_ptr = head + index;
		index += 2;

		dst_addr_ptr = head + index;
		index += 4;

		// We don't accept connections with a USERID field.
		if(head[index]){
			report_error("parse_socks_request(): USERID found, but not supported by this implementation.");
			return(-1);
		}

		// Step through and grab the domain_name, if this is 4a.
		index++;
		if( \
				!dst_addr_ptr[0] && \
				!dst_addr_ptr[1] && \
				!dst_addr_ptr[2] && \
				dst_addr_ptr[3] \
			){

			if(!(index < size)){
				return(CON_SOCKS_INIT);
			}

			domain_name = head + index;
			while(head[index]){
				index++;
				if(index == size){
					return(CON_SOCKS_INIT);
				}
			}

			atype = 0x03;
			if((cur_connection_node->rhost_rport = addr_to_string(atype, domain_name, dst_port_ptr, strlen(domain_name))) == NULL){
				report_error("parse_socks_request(): addr_to_string(%d, %lx, %lx, %d): %s", \
						atype, (unsigned long) dst_addr_ptr, (unsigned long) dst_port_ptr, (int) strlen(domain_name), strerror(errno));
				return(-1);
			}

			index++;
			cur_connection_node->buffer_ptr = head + index;

			return(CON_ACTIVE);
		}

		if((cur_connection_node->rhost_rport = addr_to_string(atype, dst_addr_ptr, dst_port_ptr, 0)) == NULL){
			report_error("parse_socks_request(): addr_to_string(%d, %lx, %lx, 0): %s", \
					atype, (unsigned long) dst_addr_ptr, (unsigned long) dst_port_ptr, strerror(errno));
			return(-1);
		}

		cur_connection_node->buffer_ptr = head + index;

		return(CON_ACTIVE);

	}else if(head[index] == 5){
		// SOCKS 5

		if(cur_connection_node->state == CON_SOCKS_INIT){
			index += 1;	
			if(!(index < size)){
				return(CON_SOCKS_INIT);
			}

			noauth_found = 0;
			nmethods = head[index++];
			for(i = 0; i < nmethods; i++){

				if(!(index < size)){
					return(CON_SOCKS_INIT);
				}

				// Someday we might implement more than the "NO AUTHENTICATION REQUIRED" method...
				if(head[index] == 0x00){
					noauth_found = 1;
				}

				index++;
			}

			cur_connection_node->buffer_ptr = head + index;
			if(noauth_found){
				return(CON_SOCKS_V5_AUTH);
			}else{
				report_error("parse_socks_request(): No supported auth type found. Only \"No Authentication Required\" is supported.");
				return(-1);
			}

		}else if(cur_connection_node->state == CON_SOCKS_V5_AUTH){

			/*
				 +----+-----+-------+------+----------+----------+
				 |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
				 +----+-----+-------+------+----------+----------+
				 | 1  |  1  | X'00' |  1   | Variable |    2     |
				 +----+-----+-------+------+----------+----------+
				 (First byte of "Variable" is the strlen of the string that follows in the DOMAINNAME case. No '\0' terminator.)

				 1 + 1 + 1 + 1 + 1
				 = 5  Minimum number of bytes before we can do anything interesting.
			 */

			if(size < 7){
				return(CON_SOCKS_INIT);
			}

			index += 1;

			if(head[index] != 1){
				report_error("parse_socks_request(): Unsupported CMD. Only CMD \"Connect\" is supported.");
			}
			index += 2;

			atype = head[index];
			index += 1;

			if(atype == 0x01){
				len = 4;
				// From the diagram above. 4 + Variable + 2, where Variable is 4 in the ipv4 case.
				if(size < (4 + len + 2)){
					return(CON_SOCKS_INIT);
				}

				dst_addr_ptr = head + index;
				index += len;
				dst_port_ptr = head + index;

			}else if(atype == 0x03){
				len = head[index];

				// From the diagram above. 4 + Variable + 2, where Variable length is defined in the first octet.
				if(size < (4 + 1 + len + 2)){
					return(CON_SOCKS_INIT);
				}

				index++;  // Move past the length variable.
				dst_addr_ptr = head + index;
				index += len;
				dst_port_ptr = head + index;

			}else if(atype == 0x04){
				len = 16;

				// From the diagram above. 4 + Variable + 2, where Variable is 16 in the ipv6 case.
				if(size < (4 + len + 2)){
					return(CON_SOCKS_INIT);
				}

				dst_addr_ptr = head + index;
				index += len;
				dst_port_ptr = head + index;

			}else{
				report_error("parse_socks_request(): atype 0x%02x unknown.", atype);
				return(-1);
			}

			index += 2;


			if((cur_connection_node->rhost_rport = addr_to_string(atype, dst_addr_ptr, dst_port_ptr, len)) == NULL){
				report_error("parse_socks_request(): addr_to_string(%d, %lx, %lx, 0): %s", \
						atype, (unsigned long) dst_addr_ptr, (unsigned long) dst_port_ptr, strerror(errno));
				return(-1);
			}

			cur_connection_node->buffer_ptr = head + index;

			return(CON_ACTIVE);
		}
	}

	return(-1);
}


// If atype is 0x03, then len is the length of the addr string.
// If atype isn't 0x03, len is ignored.
char *addr_to_string(int atype, char *addr, char *port, int len){

	char *ptr;
	unsigned short int port_num = ntohs(*((unsigned short int *)(port)));

	// strlen("255.255.255.255:65535") -> 21
	int string_len = 21;

	if(atype == 0x03){
		string_len = len;
		// strlen(":65535") -> 6
		string_len += 6;

	}else if(atype == 0x04){
		// strlen("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535") -> 47 
		string_len = 47;
	}

	if((ptr = (char *) calloc(string_len + 1, sizeof(char))) == NULL){
		report_error("addr_to_string(): calloc(%d, %d): %s", string_len + 1, (int) sizeof(char), strerror(errno));
		return(NULL);
	}

	string_len++;
	if(atype == 0x03){
		snprintf(ptr, string_len, "%s:%d", addr, port_num);
	}else if(atype == 0x04){
		snprintf(ptr, string_len, "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%d", \
				(unsigned char) addr[0], (unsigned char) addr[1], (unsigned char) addr[2], (unsigned char) addr[3], \
				(unsigned char) addr[4], (unsigned char) addr[5], (unsigned char) addr[6], (unsigned char) addr[7], \
				(unsigned char) addr[8], (unsigned char) addr[9], (unsigned char) addr[10], (unsigned char) addr[11], \
				(unsigned char) addr[12], (unsigned char) addr[13], (unsigned char) addr[14], (unsigned char) addr[15], \
				port_num);
	}else{
		snprintf(ptr, string_len, "%d.%d.%d.%d:%d", (unsigned char) addr[0], (unsigned char) addr[1], (unsigned char) addr[2], (unsigned char) addr[3], port_num);
	}

	return(ptr);
}