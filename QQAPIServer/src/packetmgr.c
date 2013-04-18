/*
 *  packetmgr.c
 *
 *  QQ Packet Management
 *
 *  Copyright (C) 2008  Huang Guan
 *
 *  2008-7-12 Created.
 *
 *  Description: This file mainly includes the functions about
 *  creating/deleting packets, sending/receiving packets
 *
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __WIN32__
#include <winsock.h>
#include <wininet.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "qqdef.h"
#include "memory.h"
#include "debug.h"
#include "qqclient.h"
#include "qqpacket.h"
#include "qqsocket.h"
#include "packetmgr.h"
#include "protocol.h"


static void delete_func(const void *p)
{
	if( p ){
		DEL( ((qqpacket*)p)->buf );	DEL( p );
	}
}

qqpacket* packetmgr_new_packet( qqclient* qq )
{
	qqpacket* p;
	NEW( p, sizeof(qqpacket) );
	if( !p ){
		DBG("Error: No enough memory.");
		return NULL;
	}
	p->time_create = p->time_alive = time( NULL );
	NEW( p->buf, sizeof(bytebuffer) );
	if( !p->buf ){
		DBG("Error: No enough memory.");
		return NULL;
	}
	p->buf->size = PACKET_SIZE;
	p->buf->pos = 0;
	p->need_ack = 1;
	p->match = NULL;
	return p;
}

qqpacket* packetmgr_new_send( qqclient* qq, int cmd )
{
//	qqpacketmgr* mgr = &qq->packetmgr;
	qqpacket* p = packetmgr_new_packet( qq );
	if( !p ){
		DBG("Error: No packet allocated.");
		return NULL;
	}
	p->head = 0x02;
	p->tail = 0x03;
	p->command = cmd;
	p->version = qq->version;
	p->seqno = qq->seqno;	//mgr->seqno[(p->command%MAX_COMMAND)]++;
	packetmgr_inc_seqno( qq );
	p->buf->pos = 0;
	return p;
}

void packetmgr_inc_seqno( qqclient* qq )
{
	qq->seqno = rand();
}

void packetmgr_new_seqno( qqclient* qq )
{
	qq->seqno = (rand()&0xF)<<3;
}

void packetmgr_del_packet( qqpacketmgr* mgr, qqpacket* p )
{
	if( p->match ){
		loop_remove( &mgr->sent_loop, p->match );
		delete_func( p->match );
	}
	delete_func( p );
}

static void check_ready_packets( qqclient* qq )
{
    if (qq->sclock)
    {
        // x
        OPS("### @ TIMEC : Others are Processing it.");
        return;
    }
    qq->sclock = 1;
	qqpacketmgr* mgr = &qq->packetmgr;
	if( !loop_is_empty(&mgr->sent_loop) || loop_is_empty(&mgr->ready_loop) )
	{
	    goto QUIT;
	}
	//if tcp, we send one by one, otherwise send them all
	if( loop_is_empty(&mgr->sent_loop) ) //good luck, get a packet and send it.
	{
		qqpacket* p = loop_pop_from_head( &mgr->ready_loop );
		if( p && p->buf ){
			//remove p from ready packets
			if( p->head!=0x02 || p->tail !=0x03 ){
				DBG("Fatal error: p->command=%x p->head: %x p->tail: %x", p->command, p->head,
					p->tail );
				delete_func( p );
				goto QUIT;
			}
            //if( p->command == QQ_CMD_SEND_IM )
            if( p->command != 0 )
            {
                // check and limit the message sends
                int tsl_count = loop_count(&mgr->timeline);
                if (tsl_count > 0)
                {
                    int tsl_last_send = (void**)loop_pop_from_tail(&mgr->timeline);
                    if (time(NULL) - tsl_last_send < 2)
                    {
                        OPS("### @ TIMEC : [%x] Last send is less then 1 sec , returned.", p->command);
                        loop_push_to_head(&mgr->ready_loop, p);
                        goto QUIT;
                    }
                    if (tsl_count > 14)
                    {
                        int tsl_first_send = (void**)loop_pop_from_head(&mgr->timeline);
                        if (time(NULL) - tsl_first_send < 59)
                        {
                            OPS("### @ TIMEC : [%x] Last 15th send is less then 1 min , returned.", p->command);
                            loop_push_to_head(&mgr->ready_loop, p);
                            goto QUIT;
                        }
                    }
                }
                // put to last of loop
                loop_push_to_tail(&mgr->timeline, (void*)time(NULL));
                // end check
            }
            OPS("### @ Package : Begin to send: %x. SN[%x],Sock[%d],Thread[%d]", p->command, p->seqno, qq->socket, pthread_self());
            mgr->send_packets ++ ;
			int ret = qqsocket_send( qq->socket, p->buf->data, p->buf->len );
			if( ret != p->buf->len ){
				DBG("Send packet failed. ret(%d)!=p->len(%d)", ret, p->buf->len );
				delete_func( p );
				qqclient_set_process( qq, P_ERROR );
			}else{
				if( p->need_ack ){
				    p->time_alive = time(NULL);
					loop_push_to_tail( &mgr->sent_loop, p );
				}else{
					delete_func( p );
				}
			}
		}else{
			DBG("no packet. ");
		}
	}
QUIT:
	qq->sclock = 0;
}

int packetmgr_put( qqclient* qq, qqpacket* p )
{
//	qqpacketmgr* mgr = &qq->packetmgr;
	return packetmgr_put_urge( qq, p, 0 );
}

//put in queue
int packetmgr_put_urge( qqclient* qq, qqpacket* p, int urge )
{
	qqpacketmgr* mgr = &qq->packetmgr;
	p->time_alive = time(NULL);
	p->send_times ++;
	//ok, send now
	if( pthread_equal( pthread_self(), mgr->thread_recv ) ){
		//recv thread
		//loop_push_to_tail( &mgr->temp_loop, p );
		// 原来的是压到temp队列，现在压到ready队列
		//loop_push_to_tail( &mgr->ready_loop, p );
		DBG("new loops from recv thread, cmd: %x", p->command);
	}else{
		//loop_push_to_tail( &mgr->ready_loop, p );
		//check_ready_packets( qq );
		DBG("new loops from [%d], cmd: %x", pthread_self(), p->command);
	}
	loop_push_to_tail( &mgr->ready_loop, p );
	return 0;
}


static int match_searcher( const void* p, const void* v )
{
	return ( ((qqpacket*)p)->command == ((qqpacket*)v)->command &&
		((qqpacket*)p)->seqno == ((qqpacket*)v)->seqno );
}
static qqpacket* match_packet( qqpacketmgr* mgr, qqpacket* p )
{
	qqpacket* m;
	m = loop_search( &mgr->sent_loop, (void*)p, match_searcher );
	return m;
}


static int repeat_searcher( const void* p, const void* v )
{
	return ( p == v );
}

int handle_packet( qqclient* qq, qqpacket* p, uchar* data, int len )
{
	qqpacketmgr* mgr = &qq->packetmgr;
	bytebuffer* buf;
	NEW( buf, sizeof( bytebuffer ) );
	if( !buf ){
		DBG("error: no enough memory to allocate buf.");
		return -99;
	}
	buf->pos = 0;
	buf->len = buf->size = len;
	memcpy( buf->data, data, buf->len );
	//get packet info
	if( qq->network == TCP || qq->network == PROXY_HTTP )
		get_word( buf );	//packet len
	p->head = get_byte( buf );
	p->tail = buf->data[buf->len-1];
	if( p->head != 0x02 || p->tail!=0x03 || buf->len > 2000 ){
		DBG("[%u] wrong packet. len=%d  head: %d  tail: %d", qq->number, buf->len, p->head, p->tail );
		DEL( buf );
		return -1;
	}
	p->version = get_word( buf );
	p->command = get_word( buf );
	p->seqno = get_word( buf );
	uint chk_repeat = (p->seqno<<16)|p->command;
	//
	p->match = match_packet( mgr, p );
	// check repeat
	if( loop_search( &mgr->recv_loop, (void*)chk_repeat, repeat_searcher ) == NULL ){
		loop_push_to_tail( &mgr->recv_loop, (void*)chk_repeat );
		p->time_alive = time(NULL);
		//do a test
		if ( !p->buf ){
			DBG("%u: Error impossible. p->buf: %x  p->command: %x", qq->number, p->buf, p->command );
		}
		//deal with the packet
		process_packet( qq, p, buf );
		// 不再获取temp队列的包发送
		/*
		qqpacket* t;
		while( (t = loop_pop_from_tail( &mgr->temp_loop )) ){
            DBG("get a temp package: %x", t->command);
            loop_push_to_head( &mgr->ready_loop, t );
		}
		*/
		mgr->failed_packets = 0;
	}
	else
	{
        mgr->repeat_packets ++ ;
		DBG("repeated packet! cmd: %x, seqno: %x", p->command, p->seqno );
	}
	//
    if( p->match ){
        loop_remove( &mgr->sent_loop, p->match );
        delete_func( p->match );
    }
    p->match = NULL;
    //
	DEL( buf );
	//check_ready_packets( qq );
	return 0;
}

void* packetmgr_recv( void* data )
{
	int ret;
	qqpacket* p;
	qqclient* qq = (qqclient*)data;
	qqpacketmgr* mgr = &qq->packetmgr;
	uchar* recv_buf;
	int pos;
	NEW( recv_buf, PACKET_SIZE );
	p = packetmgr_new_packet( qq );
	if( !p || !recv_buf ){
		DBG("error: p=%x  buf=%x", p, recv_buf );
		DEL( p ); DEL( recv_buf );
		return NULL;
	}
	pos = 0;
	while( qq->process != P_INIT ){
		ret = qqsocket_recv( qq->socket, recv_buf, PACKET_SIZE-pos );
		if( ret <= 0 ){
			if( qq->process != P_INIT ){
				//DBG("ret=%d", ret );
				SLEEP(1);
				continue;
			}else{
				break;
			}
		}
		OPS("### @ Package : Recv new package, len is %d", ret);
		mgr->recv_packets ++ ;
		pos += ret;
		//TCP only
		if( qq->network == TCP || qq->network == PROXY_HTTP ){
			if ( pos > 2 ){
				if( !qq->proxy_established && qq->network == PROXY_HTTP ){
					if( strstr( (char*)recv_buf, "200" )!=NULL ){
						DBG("proxy server reply ok!");
						qq->proxy_established = 1;
						prot_login_touch( qq );
						//手动添加到发送队列。
						qqpacket* t;
						while( (t = loop_pop_from_tail( &mgr->temp_loop )) )
							loop_push_to_head( &mgr->ready_loop, t );
						//检查待发送包
						//check_ready_packets( qq );
					}else{
						DBG("proxy server reply failure!");
						recv_buf[ret]=0;
						DBG( "%s", recv_buf );
						qqclient_set_process( qq, P_ERROR );
					}
				}else{
					int len = ntohs(*(ushort*)recv_buf);
					if( pos >= len )	//a packet is O.K.
					{
						if( handle_packet( qq, p, recv_buf, len ) < 0 ) {
							pos = 0;
							continue;
						}
						pos -= len;
						//copy data to buf
						if( pos > 0 ){
							memmove( recv_buf, recv_buf+len, pos );
						}
					}else if( pos == PACKET_SIZE ){
						DBG("error: pos: 0x%x ", pos );
						pos = 0;
					}
				}
			}
		}else{	//UDP
			handle_packet( qq, p, recv_buf, ret );
			pos = 0;
		}
	}
	DEL( recv_buf );
	packetmgr_del_packet( mgr, p );
	DBG("end.");
	return NULL;
}

int packetmgr_start( qqclient* qq )
{
	int ret;
	qqpacketmgr *mgr = &qq->packetmgr;
	memset( mgr, 0, sizeof(qqpacketmgr) );
	loop_create( &mgr->recv_loop, MAX_LOOP_PACKET, NULL );
	loop_create( &mgr->ready_loop, 128, delete_func );
	loop_create( &mgr->temp_loop, 64, delete_func );
	loop_create( &mgr->sent_loop, 32, delete_func );
	loop_create( &mgr->timeline, 15, NULL);
	ret = pthread_create( &mgr->thread_recv, NULL, packetmgr_recv, (void*)qq );
	return 0;
}


void packetmgr_end( qqclient* qq )
{
	qqpacketmgr *mgr = &qq->packetmgr;
	pthread_join( mgr->thread_recv, NULL );
	loop_cleanup( &mgr->recv_loop );
	loop_cleanup( &mgr->sent_loop );
	loop_cleanup( &mgr->ready_loop );
	loop_cleanup( &mgr->temp_loop );
	loop_cleanup( &mgr->timeline );
	DBG("packetmgr_end");
}

static int timeout_searcher( const void* p, const void* v )
{
	if( (time_t)v-((qqpacket*)p)->time_alive > 0 ){
		return 1;
	}else{
		return 0;
	}
}
int packetmgr_check_packet( struct qqclient* qq, int timeout )
{
	qqpacketmgr *mgr = &qq->packetmgr;
	qqpacket* p;
	if (qq->process == 0) return 0;
	time_t timeout_time = time(NULL) - timeout;
	//when locked, cannot recv packet till unlock.
	do{
		p = loop_search( &mgr->sent_loop, (void*)timeout_time, timeout_searcher );
		if( p ){
			loop_remove( &mgr->sent_loop, p );
		}
		if( p ){
            OPS("### @ Package : Alives %d, creates %d .Movs[%d].", p->time_alive, p->time_create, p->time_alive - p->time_create);
			if( p->send_times >= 5 )
			{
				if( p->command == QQ_CMD_SEND_IM ){
                    buddy_msg_callback( qq, 10000, time(NULL), "messages.send.failed" );
					OPS("### @ SYS : Some messages send Failed.");
				}
				//make an event to tell the program that we have a problem.
				char event[64];
				sprintf( event, "sendfailed^$%d^$%d", p->command, p->seqno );
				qqclient_put_event( qq, event );
				mgr->failed_packets ++ ;
				mgr->loss_packets ++ ;
				//To avoid too many failed packets, just shut it down.
				if( mgr->failed_packets > 30 || qq->process != P_LOGIN ){
					qqclient_set_process( qq, P_ERROR );
				}
				OPS("### @ Package : Loss the package: %x, SN:%x", p->command, p->seqno);
			}
			else
			{
				OPS("### @ Package : Resending packet: %x, SN:%x", p->command, p->seqno);
				packetmgr_put_urge( qq, p, 1 );
			}
		}
	}while( 0 && p );
	check_ready_packets(qq);
	return 0;
}

#define INTERVAL 1
void packetmgr_daemon( void* data )
{
	qqclient* qq = (qqclient*) data;
	int sleepintv = 200/INTERVAL;
	int counter = 1;
	while( 1 ){
		counter ++;
		if( counter % INTERVAL == 0 ){
			packetmgr_check_packet( qq, 10 );
		}
		USLEEP( sleepintv );
	}
	return;
}
