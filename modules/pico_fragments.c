/*********************************************************************
   PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
   See LICENSE and COPYING for usage.

   Authors: Ludo Mondelaers
 *********************************************************************/


#include "pico_config.h"
#ifdef PICO_SUPPORT_IPV6
#include "pico_ipv6.h"
#include "pico_icmp6.h"
#endif
#ifdef PICO_SUPPORT_IPV4
#include "pico_ipv4.h"
#include "pico_icmp4.h"
#endif
#include "pico_stack.h"
#include "pico_eth.h"
#include "pico_udp.h"
#include "pico_tcp.h"
#include "pico_socket.h"
#include "pico_device.h"
#include "pico_tree.h"
#include "pico_constants.h"

/*** macros ***/

#define PICO_IP_FRAG_TIMEOUT 60000
#define IPFRAG_DEBUG
#ifdef IPFRAG_DEBUG
#  define frag_dbg  printf
#else
# define frag_dbg(...) do{}while(0);
#endif


//PICO_IPV4_MTU

/*** Type definitions ***/

typedef struct 
{
    // uniquely identify fragments by: (RFC 791 & RFC 2460)
    uint32_t            frag_id;
    uint8_t             proto; //
    union pico_address  src; 
    union pico_address  dst;

//    PICO_TREE_DECLARE(holes, hole_compare); // this macro contains an initialisation to a global variable: can not use it here 
    struct pico_tree    holes;
 
    struct pico_frame * frame;
    pico_time           expire;
    
	uint32_t 			net_hdr_offset;
	uint32_t 			transport_hdr_offset;
    
}pico_fragment_t;

typedef     struct
{
    uint16_t first;
    uint16_t last;
}pico_hole_t;

/***  Prototypes ***/

static int fragments_compare(void *fa, void *fb);   /*pico_fragment_t*/
static int hole_compare(void *a, void *b);          /*pico_hole_t*/
// alloc and free of fragment tree
static pico_fragment_t *pico_fragment_alloc( uint16_t iphdrsize, uint16_t bufsize);
static pico_fragment_t *pico_fragment_free(pico_fragment_t * fragment);

static int pico_fragment_arrived(pico_fragment_t* fragment, struct pico_frame* frame, uint16_t byte_offset, uint16_t more_flag );
// alloc and free for the hole tree
static pico_hole_t* pico_hole_free(pico_hole_t *hole);
static pico_hole_t* pico_hole_alloc(uint16_t first,uint16_t last);

/*** static declarations ***/
//static     PICO_TREE_DECLARE(ip_fragments, fragments_compare);
static     struct pico_tree    pico_fragments = { &LEAF, fragments_compare};
// our timer: allocoate one instance
static struct pico_timer*      pico_fragment_timer = NULL;



/*** global function called from pico_ipv6.c ***/

#ifdef PICO_SUPPORT_IPV6
// byte offset and more flag from exthdr (RFC2460)
#define IP6FRAG_OFF(frag)  ((frag & 0xFFF8))
#define IP6FRAG_MORE(frag) ((frag & 0x0001) ? 1 : 0)

#define IP6FRAG_ID(exthdr) ((uint32_t)((exthdr->ext.frag.id[0] << 24)   |   \
                                       (exthdr->ext.frag.id[1] << 16)   |   \
                                       (exthdr->ext.frag.id[2] << 8)    |   \
                                        exthdr->ext.frag.id[3]))



static int copy_ipv6_exthdr_nofrag(struct pico_frame* dst, struct pico_frame* src)
{
#if 1
    int done = 0;
    struct pico_ipv6_hdr *srchdr = (struct pico_ipv6_hdr *)src->net_hdr;
    struct pico_ipv6_hdr *dsthdr = (struct pico_ipv6_hdr *)dst->net_hdr;
    int srcidx = 0;
    int dstidx = 0;
    uint8_t nxthdr = srchdr->nxthdr;
    uint8_t* pdstnxthdr = &dsthdr->nxthdr;
    // parse ext hdrs
    while(!done)
    {
		frag_dbg("[LUM:%s:%d] nxthdr:%d %s\n", __FILE__,__LINE__,nxthdr,
				nxthdr == PICO_IPV6_EXTHDR_DESTOPT  ? "PICO_IPV6_EXTHDR_DESTOPT":
        		nxthdr == PICO_IPV6_EXTHDR_ROUTING	? "PICO_IPV6_EXTHDR_ROUTING":
        		nxthdr == PICO_IPV6_EXTHDR_HOPBYHOP ? "PICO_IPV6_EXTHDR_HOPBYHOP":
        		nxthdr == PICO_IPV6_EXTHDR_ESP      ? "PICO_IPV6_EXTHDR_ESP":
        		nxthdr == PICO_IPV6_EXTHDR_AUTH		? "PICO_IPV6_EXTHDR_AUTH":
        		nxthdr == PICO_IPV6_EXTHDR_NONE		? "PICO_IPV6_EXTHDR_NONE":
        		nxthdr == PICO_PROTO_TCP			? "PICO_PROTO_TCP":
        		nxthdr == PICO_PROTO_UDP			? "PICO_PROTO_UDP":
        		nxthdr == PICO_PROTO_ICMP6			? "PICO_PROTO_ICMP6":
				nxthdr == PICO_ICMP6_ECHO_REQUEST	? "PICO_ICMP6_ECHO_REQUEST":
				nxthdr == PICO_ICMP6_DEST_UNREACH	? "PICO_ICMP6_DEST_UNREACH":
				nxthdr == PICO_ICMP6_PKT_TOO_BIG	? "PICO_ICMP6_PKT_TOO_BIG":
				nxthdr == PICO_ICMP6_ECHO_REPLY		? "PICO_ICMP6_ECHO_REPLY":
				nxthdr == PICO_ICMP6_ROUTER_SOL		? "PICO_ICMP6_ROUTER_SOL":
				nxthdr == PICO_ICMP6_ROUTER_ADV		? "PICO_ICMP6_ROUTER_ADV":
				nxthdr == PICO_ICMP6_NEIGH_SOL		? "PICO_ICMP6_NEIGH_SOL":
				nxthdr == PICO_ICMP6_NEIGH_ADV		? "PICO_ICMP6_NEIGH_ADV":
				nxthdr == PICO_ICMP6_REDIRECT		? "PICO_ICMP6_REDIRECT":
				"unknown");

        switch(nxthdr)
        {
        case PICO_IPV6_EXTHDR_DESTOPT:
        case PICO_IPV6_EXTHDR_ROUTING:
        case PICO_IPV6_EXTHDR_HOPBYHOP:
        case PICO_IPV6_EXTHDR_ESP:
        case PICO_IPV6_EXTHDR_AUTH:
        {
            uint8_t len = (uint8_t)(srchdr->extensions[srcidx+1] << 3); 
frag_dbg("[LUM:%s:%d] nxthdr:%d len:%d pdstnxthdr:%p\n", __FILE__,__LINE__,nxthdr,len,pdstnxthdr);
            memcpy(&dsthdr->extensions[dstidx],&srchdr->extensions[srcidx],(size_t)len);
            srcidx += len;
            dstidx += len;
            *pdstnxthdr = nxthdr;
            pdstnxthdr = &dsthdr->extensions[dstidx];
        }
        break;
        case PICO_IPV6_EXTHDR_FRAG: 
            srcidx += 8;            // remove frag field from dsthdr
        break;
        case PICO_IPV6_EXTHDR_NONE:
        case PICO_PROTO_TCP:
        case PICO_PROTO_UDP:
        case PICO_PROTO_ICMP6:
        /*
		case PICO_ICMP6_ECHO_REQUEST:
		case PICO_ICMP6_DEST_UNREACH:
		case PICO_ICMP6_PKT_TOO_BIG:
		case PICO_ICMP6_ECHO_REPLY:
		case PICO_ICMP6_ROUTER_SOL:
		case PICO_ICMP6_ROUTER_ADV:
		case PICO_ICMP6_NEIGH_SOL:
		case PICO_ICMP6_NEIGH_ADV:
		case PICO_ICMP6_REDIRECT:
		*/
/*
#define PICO_ICMP6_PKT_TOO_BIG         2
#define PICO_ICMP6_TIME_EXCEEDED       3
#define PICO_ICMP6_PARAM_PROBLEM       4
		
#define PICO_ICMP6_ECHO_REPLY          129
#define PICO_ICMP6_ROUTER_SOL          133
#define PICO_ICMP6_ROUTER_ADV          134
#define PICO_ICMP6_NEIGH_SOL           135
#define PICO_ICMP6_NEIGH_ADV           136
#define PICO_ICMP6_REDIRECT            137
*/

        
            *pdstnxthdr = nxthdr;
            done=1;
        break;
        default:
        /* Invalid next header */
			frag_dbg("[LUM:%s:%d] unrecognised nxthdr:%d \n",__FILE__,__LINE__,nxthdr);
            pico_icmp6_parameter_problem(src, PICO_ICMP6_PARAMPROB_NXTHDR, (uint32_t)nxthdr);
            done=1;
        break;
        }
        nxthdr = srchdr->extensions[srcidx];   // advance pointer
    }
    dst->payload = &dsthdr->extensions[dstidx];  
    dst->transport_hdr = dst->payload;
#else
#endif
    return dstidx;
}


extern void pico_ipv6_process_frag(struct pico_ipv6_exthdr *exthdr, struct pico_frame *f, uint8_t proto /* see pico_addressing.h */)
{
    int retval = 0;
    if(exthdr && f)
    {
		struct pico_ipv6_hdr*ip6hdr=(struct pico_ipv6_hdr*)f->net_hdr;
		union pico_address src = {0};
        union pico_address dst = {0};

        // does the fragment already has its fragment tree?
        pico_fragment_t key;
        pico_fragment_t *fragment = NULL;

		src.ip6 = ip6hdr->src;
        dst.ip6 = ip6hdr->dst;

        memset(&key,0,sizeof(pico_fragment_t));
        key.frag_id = IP6FRAG_ID(exthdr);
        key.proto = proto;

        key.src = src; //src ip6
        key.dst = dst;   // dst ip6
        
        fragment = pico_tree_findKey( &pico_fragments,  &key); 
        if(!fragment)  // this is a new frag_id
        {
            // allocate fragment tree
            fragment = pico_fragment_alloc( PICO_SIZE_ETHHDR + PICO_SIZE_IP6HDR, 2*PICO_IPV6_MIN_MTU + 64 /*max lenght of options RFC815*/);

			frag_dbg("[LUM:%s:%d] frag_id not found in fragment tree. frag_id:0x%X \n",__FILE__,__LINE__,IP6FRAG_ID(exthdr));

            if(fragment)
            {

                if(IP6FRAG_OFF(f->frag) == 0)  // offset is 0
                {
                    // if first frame: copy options  see RFC815
                    //fragment->start_payload = PICO_SIZE_IP6HDR;
                }
                else  
                {
                    //fragment is not the first fragment: assume no options, and add them later
                    //fragment->start_payload = PICO_SIZE_IP6HDR;
                }
                
                // copy ip hdr
                if(fragment->frame->net_hdr && f->net_hdr)
                {
                    //memcpy(fragment->frame->net_hdr,f->net_hdr,/*PICO_SIZE_ETHHDR +*/ PICO_SIZE_IP6HDR);
                    memcpy(fragment->frame->buffer,f->buffer, PICO_SIZE_ETHHDR + PICO_SIZE_IP6HDR);
                }
                // copy ext hdr
                copy_ipv6_exthdr_nofrag(fragment->frame, f);   // copy exthdr but not the frag option
                // copy payload
                memcpy(fragment->frame->transport_hdr,f->transport_hdr,f->transport_len);
                
                

                fragment->frag_id = IP6FRAG_ID(exthdr);
                fragment->proto = proto;
				fragment->src.ip6 = src.ip6;
				fragment->dst.ip6 = dst.ip6;
                
                fragment->holes.compare = hole_compare;
                fragment->holes.root = &LEAF; 
                
                pico_tree_insert(&pico_fragments, fragment);

            }
        }
        if(fragment)
        {
            retval = pico_fragment_arrived(fragment, f, IP6FRAG_OFF(f->frag), IP6FRAG_MORE(f->frag) );
//            if(retval < 1)
//            {
//                pico_frame_discard(f);
//                f=NULL;
//            }
        }
    }
}
#endif

#ifdef PICO_SUPPORT_IPV4


#define IP4FRAG_ID(hdr) (hdr->id)

// byte offset and more flag from iphdr (RFC791)
#define IP4FRAG_OFF(frag)  (((uint32_t)frag & PICO_IPV4_FRAG_MASK) << 3ul)
#define IP4FRAG_MORE(frag) ((frag & PICO_IPV4_MOREFRAG) ? 1 : 0)


extern int pico_ipv4_process_frag(struct pico_ipv4_hdr *hdr, struct pico_frame *f, uint8_t proto /* see pico_addressing.h */)
{
    int retval = 0;
    if(hdr && f)
    {
        // does the fragment already has its fragment tree?
        pico_fragment_t key;
        pico_fragment_t *fragment = NULL;

        memset(&key,0,sizeof(pico_fragment_t));
        key.frag_id = short_be(IP4FRAG_ID(hdr));
        key.proto = proto;

        fragment = pico_tree_findKey( &pico_fragments,  &key); 

        frag_dbg("[LUM:%s:%d] Searching for frag_id:0x%X proto:%d(%s): %s \n",
                    __FILE__,__LINE__,
                    key.frag_id,
                    key.proto,
                        (proto == PICO_PROTO_IPV4)  ? "PICO_PROTO_IPV4" :
                        (proto == PICO_PROTO_ICMP4) ? "PICO_PROTO_ICMP4" :
                        (proto == PICO_PROTO_IGMP)  ? "PICO_PROTO_IGMP" :
                        (proto == PICO_PROTO_TCP)   ? "PICO_PROTO_TCP" :
                        (proto == PICO_PROTO_UDP)   ? "PICO_PROTO_UDP" :
                        (proto == PICO_PROTO_IPV6)  ? "PICO_PROTO_IPV6" :
                        (proto == PICO_PROTO_ICMP6) ? "PICO_PROTO_ICMP6" :  "unknown",
                    fragment?"FOUND":"NOT FOUND");

        if(!fragment)  // this is a new frag_id
        {
            // allocate fragment tree
            fragment = pico_fragment_alloc( PICO_SIZE_IP4HDR, /*2**/PICO_IPV4_MTU + 64 /*max length of options*/);
            if(fragment)
            {
                if(IP4FRAG_OFF(f->frag) == 0)
                {
                    // if first frame: TODO copy options  see RFC815
                    //fragment->start_payload = PICO_SIZE_IP4HDR;
                }
                else  
                {
                    //fragment is not the first fragment: assume no options, and add them later
                    //fragment->start_payload = PICO_SIZE_IP4HDR;
                }
                //TODO: copy ext + clear frag options
                if(fragment->frame->net_hdr &&  f->net_hdr)
                {
                    memcpy(fragment->frame->net_hdr,f->net_hdr,PICO_SIZE_ETHHDR + PICO_SIZE_IP4HDR);
                }
                else
                {
                    frag_dbg("[%s:%d] fragment->frame->net_hdr:%p f->net_hdr:%p PICO_SIZE_ETHHDR + PICO_SIZE_IP4HDR:%d);",__FILE__,__LINE__,fragment->frame->net_hdr,f->net_hdr,PICO_SIZE_ETHHDR + PICO_SIZE_IP4HDR);
                }

                fragment->frag_id = key.frag_id;
                fragment->frame->frag = 0;  // remove frag options
                fragment->frame->proto = proto;
                
                fragment->proto = proto;
                {
                    struct pico_ipv4_hdr*ip4hdr=(struct pico_ipv4_hdr*)f->net_hdr;
                    fragment->src.ip4 = ip4hdr->src;
                    fragment->dst.ip4 = ip4hdr->dst;
                }

                fragment->holes.compare = hole_compare;
                fragment->holes.root = &LEAF; 
                
                pico_tree_insert(&pico_fragments, fragment);
            }
        }
        if(fragment)
        {
            //  hdr is stored in network order !!! oh crap
            uint16_t offset = IP4FRAG_OFF(short_be(hdr->frag));
            uint16_t more   = IP4FRAG_MORE(short_be(hdr->frag));

            retval = pico_fragment_arrived(fragment, f, offset, more);
            if(retval < 1)  // the original frame is re-used when retval ==1 , so dont discard here
            {
                pico_frame_discard(f);
                f=NULL;
            }
        }
    }
    return retval;
}
#endif



static int fragments_compare(void *a, void *b)
{
    pico_fragment_t *fa = a;
    pico_fragment_t *fb = b;
    int retval=0;
    if(fa && fb)
    {                                                             
        if((retval = (fa->frag_id - fb->frag_id)) == 0)    // fragid
        {
            if((retval = (fa->proto - fb->proto)) == 0)  // and protocol
            {
#if 1				
                if((fa->proto == PICO_PROTO_IPV4)  || (fa->proto == PICO_PROTO_ICMP4)  || 
                    (fa->proto == PICO_PROTO_IGMP) || (fa->proto == PICO_PROTO_TCP)    || 
                        (fa->proto == PICO_PROTO_UDP))
                {
                    if((retval = memcmp(&fa->src,&fb->src,sizeof(struct pico_ip4))) == 0) //src ip4
                    {
                        retval = memcmp(&fa->dst,&fb->dst,sizeof(struct pico_ip4));       //dst
                    }  //  source addr   & dest addr
                }
                else if ((fa->proto == PICO_PROTO_IPV6)  ||                  (fa->proto == PICO_PROTO_ICMP6)) 
                {
                    if((retval = memcmp(&fa->src,&fb->src,sizeof(struct pico_ip6))) == 0) //src ip6
                    {
                        retval = memcmp(&fa->dst,&fb->dst,sizeof(struct pico_ip6));   // dst ip6
                    }
                }
#else
     			frag_dbg("[LUM:%s:%d] src and dst ip not checked  \n",__FILE__,__LINE__);
				
#endif
            }
        }
    }
    else
    {
        retval = 0;
    }
    return retval;
}




static pico_fragment_t *pico_fragment_alloc( uint16_t iphdrsize, uint16_t bufsize )  // size = exthdr + payload (MTU)
{
    pico_fragment_t* fragment = PICO_ZALLOC(sizeof(pico_fragment_t) );

    if(fragment)
    {
        struct pico_frame* frame  = pico_frame_alloc(/*exthdr_size +*/ bufsize + iphdrsize /*+ (uint32_t)PICO_SIZE_ETHHDR*/);
        
        if(frame)
        {
#ifdef IPFRAG_DEBUG
			memset(frame->buffer, 0x55, bufsize + iphdrsize);
#endif
            frame->net_hdr = frame->buffer + PICO_SIZE_ETHHDR;
            frame->net_len = iphdrsize;
frag_dbg("[LUM:%s:%d] frame->net_len:%d  \n",__FILE__,__LINE__, frame->net_len);

            frame->transport_hdr = frame->net_hdr + iphdrsize;
            frame->transport_len = bufsize;

            frame->datalink_hdr = frame->buffer;

            fragment->net_hdr_offset = PICO_SIZE_ETHHDR;
			fragment->transport_hdr_offset = PICO_SIZE_ETHHDR + iphdrsize;


            fragment->frame = frame;
        }
    }
    return fragment;   
}


static pico_fragment_t *pico_fragment_free(pico_fragment_t * fragment)
{
    if(fragment)
    {
        struct pico_tree_node *idx=NULL;
        struct pico_tree_node *tmp=NULL;
        
        /* cancel timer */
        if(fragment->expire)
        {
            fragment->expire = 0;
        }
        
        /*empty hole tree*/
        pico_tree_foreach_safe(idx, &fragment->holes, tmp) 
        {
            pico_hole_t *hole = idx->keyValue;
            
            pico_tree_delete(&fragment->holes, hole);
            pico_hole_free(hole);
            hole = NULL;
        }

        if(fragment->frame)
        {
            /* discard frame*/
            pico_frame_discard(fragment->frame);
            fragment->frame = NULL;
        }
        pico_tree_delete(&pico_fragments, fragment);
        PICO_FREE(fragment);
    }
    return NULL;
}

/***
*
*  following functions use the hole algo as described in rfc815
*
***/



static int hole_compare(void* a,void* b)
{
    pico_hole_t *ha = (pico_hole_t *)a;
    pico_hole_t *hb = (pico_hole_t *)b;
    if(ha && hb)
    {
        return  (ha->first - hb->first);
    }
    else
    {
        return 0;
    }
}


static pico_hole_t* pico_hole_alloc(uint16_t first,uint16_t last)
{
    pico_hole_t* hole = PICO_ZALLOC(sizeof(pico_hole_t));
    if(hole)
    {
        hole->first=first;
        hole->last=last;
    }
    return hole;
}


static pico_hole_t* pico_hole_free(pico_hole_t *hole)
{
    if(hole)
    {
        PICO_FREE(hole);
        hole=NULL;
    }
    return hole;
}


static void pico_ip_frag_expired(pico_time now, void *arg)
{
    pico_fragment_t * fragment=NULL;    
    struct pico_tree_node *idx=NULL;
    struct pico_tree_node *tmp=NULL;

    (void)arg;
    pico_fragment_timer = NULL;  // timer expired
    uint32_t empty=1;
    //frag_dbg("[LUM:%s%d] inside pico_ip_frag_expired \n",__FILE__,__LINE__);
    
    pico_tree_foreach_safe(idx, &pico_fragments, tmp) 
    {
        fragment = idx->keyValue;
        if(fragment->expire < now)
        {
            frag_dbg("[%s:%d]//TODO notify ICMP \n",__FILE__,__LINE__);
            frag_dbg("[%s:%d] fragment expired:%p frag_id:0x%X \n",__FILE__,__LINE__,fragment, fragment->frag_id);
            
            pico_fragment_free(fragment);
            fragment=NULL;
        }
        empty=0;
    }
    if(!empty)  // if still fragments in the tree...
    {
        // once the timer is expired, it is removed from the queue
        // if there are still fragments in the tree, restart the timer
        pico_fragment_timer = pico_timer_add(3000, /*cleanup expired fragments every x ms*/ pico_ip_frag_expired, NULL);
        //frag_dbg("[LUM:%s:%d] added timer %p \n",__FILE__,__LINE__,pico_fragment_timer);
    }
}


#define INFINITY 55555 /* just a big number <16bits*/

// note: offset and more flag are located differently in ipv4(iphdr) and ipv6(exthdr)
// offset in expressed in octets (bytes) (not the 8 byte unit used in ip)

static int pico_fragment_arrived(pico_fragment_t* fragment, struct pico_frame* frame, uint16_t offset, uint16_t more )
{
    struct pico_frame* full=NULL;


#ifdef IPFRAG_DEBUG
	frag_dbg("[LUM:%s:%d] content of fragmented packet: %p net_len:%d transport_len:%d\n",__FILE__,__LINE__,fragment->frame->buffer,fragment->frame->net_len,fragment->frame->transport_len);
	if(1)
	{
		int i;
		for(i=0;i < fragment->frame->net_len;i=i+8)
		{
			frag_dbg("0x%04X: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X \n",i,
				frame->buffer[i+0],frame->buffer[i+1],frame->buffer[i+2],frame->buffer[i+3],
				frame->buffer[i+4],frame->buffer[i+5],frame->buffer[i+6],frame->buffer[i+7]);
		}
	}
#endif

	if(!more &&  (offset == 0))
	{
		// no need for reassemble packet
		return 1;    // process orig packet
	}

    if(fragment && frame)
    {
        pico_hole_t *first = pico_tree_first(&fragment->holes);
        
        if(first == NULL)   /*first fragment of packet arrived*/
        {
            pico_hole_t *hole = pico_hole_alloc((uint16_t)0,(uint16_t)INFINITY);
            frag_dbg("[LUM:%s:%d] first fragment of packet arrived:fragment:%p fragment->holes:%p \n",__FILE__,__LINE__,fragment,&fragment->holes);
            
            if(hole)
            {
                pico_tree_insert(&fragment->holes,hole);
            }
            fragment->expire = PICO_TIME_MS() + PICO_IP_FRAG_TIMEOUT;  // fragment expires when the packet is not complete after timeout
            if(pico_fragment_timer == NULL)
            {
                pico_fragment_timer = pico_timer_add(1000, /*cleanup expired fragments every sec*/ pico_ip_frag_expired, NULL);
            }
        }
        if(!more)    /*last fragment of packet arrived*/
        {
            // retrieve the size of the reassembled packet
            pico_hole_t* hole = pico_tree_last(&fragment->holes);
            if(hole /*&& IS_LEAF(hole)*/)
            {
                hole->last = (uint16_t)(offset + frame->transport_len);
                frag_dbg("[LUM:%s:%d] reassembled packet size:%d \n",__FILE__,__LINE__,hole->last);
                // adjust transport len
                frag_dbg("[LUM:%s:%d] before adjusted transportlen:%d \n",__FILE__,__LINE__,fragment->frame->transport_len);
                fragment->frame->transport_len = offset + frame->transport_len;
                frag_dbg("[LUM:%s:%d] after adjusted transportlen:%d \n",__FILE__,__LINE__,fragment->frame->transport_len);
                
                if(hole->first == hole->last)
                {
                    pico_tree_delete(&fragment->holes,hole);    // all done!
                }
            }
        }


        // copy the received frame into the reassembled packet
        frag_dbg("[LUM:%s:%d] offset:%d frame->transport_len:%d fragment->frame->buffer_len:%d\n",__FILE__,__LINE__,offset,frame->transport_len,fragment->frame->buffer_len);
        if( (offset + frame->transport_len) < fragment->frame->buffer_len ) // check for buffer space 
        {
            if(fragment->frame->transport_hdr && frame->transport_hdr)
            {
                //frag_dbg("[LUM:%s:%d]  Reassemble packet:      fragment:%p fragment->frame:%p fragment->frame->transport_hdr:%p frame:%p frame->transport_hdr:%p frame->transport_len:%d\n",
                //            __FILE__,__LINE__, fragment,   fragment->frame,   fragment->frame->transport_hdr,   frame,   frame->transport_hdr,   frame->transport_len);
                memcpy(fragment->frame->transport_hdr + offset , frame->transport_hdr, frame->transport_len);
                
            }
            else
            {
                // notify icmp
                pico_fragment_free(fragment);
                fragment=NULL;
            }
        }
        else
        {
            // frame->buffer is too small
            // allocate new frame and copy all
            uint32_t alloc_len= frame->buffer_len > fragment->frame->buffer_len ? 2*frame->buffer_len : 2*fragment->frame->buffer_len ;
            struct pico_frame* newframe = pico_frame_alloc( alloc_len); // make it twice as big
            struct pico_frame* oldframe = NULL;

            frag_dbg("[LUM:%s:%d] frame->buffer is too small realloc'd:%p buffer:%p \n",__FILE__,__LINE__,newframe,newframe->buffer );
            
            // copy hdrs + options + data
            if(newframe)
            {
                memcpy(newframe->buffer,fragment->frame->buffer,fragment->frame->buffer_len);
                
                // set pointers 
                fragment->frame->net_hdr       = fragment->frame->buffer + fragment->net_hdr_offset;
                fragment->frame->transport_hdr = fragment->frame->buffer + fragment->transport_hdr_offset;

                frag_dbg("[LUM:%s:%d] net_hdr:%p transport_hdr:%p\n",__FILE__,__LINE__,fragment->frame->net_hdr,fragment->frame->transport_hdr);
            
                oldframe = fragment->frame;
                fragment->frame = newframe;
                pico_frame_discard(oldframe);
                newframe=NULL;
                oldframe=NULL;
            }
            else
            {
				frag_dbg("[LUM:%s:%d] Failed to allocate frame buffer \n",__FILE__,__LINE__ );
                // discard packet: no more memory
                pico_fragment_free(fragment);
                // notify icmp
            }
        }
    }
    // do the administration of the missing holes
    if(fragment && (full=fragment->frame))
    {
        struct pico_tree_node *idx=NULL, *tmp=NULL;
        pico_hole_t *hole = NULL;
        uint16_t    frame_first = offset; 
        uint16_t    frame_last  = frame_first + frame->transport_len; 
        

        frag_dbg("[LUM:%s:%d] frame_first:%d frame_last:%d offset:%d more:%d fragment->holes:%p \n",__FILE__,__LINE__,frame_first,frame_last,offset,more,&fragment->holes );

        /*RFC 815 step 1*/
        //pico_tree_foreach_safe(index, &fragment->holes, hole) 
        pico_tree_foreach_safe(idx, &fragment->holes, tmp) 
        {
            hole = idx->keyValue;
            /*RFC 815 step 2*/
            if(frame_first > hole->last)
            {
                continue;
            }
            /*RFC 815 step 3*/
            else if(frame_last < hole->first)
            {
                continue;
            }
            /*RFC 815 step 4*/
            frag_dbg("[LUM:%s:%d] deleting hole:%d-%d \n",__FILE__,__LINE__,hole->first,hole->last);
            pico_tree_delete(&fragment->holes, hole);
            /*RFC 815 step 5*/
            if(frame_first > hole->first)
            {
                pico_hole_t *new_hole =  pico_hole_alloc(hole->first,frame_first - 1u);
                if(new_hole)
                {
                    frag_dbg("[LUM:%s:%d] inserting new hole:%d-%d \n",__FILE__,__LINE__,new_hole->first,new_hole->last);
                    pico_tree_insert(&fragment->holes, new_hole);
                }
            }
            /*RFC 815 step 6*/
            else if(frame_last < hole->last)
            {
                pico_hole_t *new_hole =  pico_hole_alloc(frame_last + 1u,hole->last);
                if(new_hole)
                {
                    frag_dbg("[LUM:%s:%d] inserting new hole:%d-%d \n",__FILE__,__LINE__,new_hole->first,new_hole->last);
                    pico_tree_insert(&fragment->holes, new_hole);
                }
            }
            /*RFC 815 step 7*/
            PICO_FREE(hole);
            hole=NULL;
        }    

#if 0 //def IPFRAG_DEBUG
        if(fragment)
        {
            struct pico_tree_node *idx2=NULL, *tmp2=NULL;
            pico_hole_t *hole2 = NULL;
            uint32_t empty=1;
    
            frag_dbg("[LUM:%s:%d] printing hole tree for fragment:%p id:0x%X fragment->holes:%p\n",__FILE__,__LINE__,fragment,fragment->frag_id,fragment->holes);
            pico_tree_foreach_safe(idx2, &fragment->holes, tmp2) 
            {
                hole2 = idx2->keyValue;
                empty=0;

                frag_dbg("[LUM:%s:%d] first:%d last:%d \n",__FILE__,__LINE__,hole2?hole2->first:0,hole2?hole2->last:0);
            }
            frag_dbg("[LUM:%s:%d] %s \n",__FILE__,__LINE__,empty?"empty":"done");
        }
#endif

        /*RFC 815 step 8*/
        if(pico_tree_empty(&fragment->holes))
        {
            /* now send the reassembled packet upstream*/
            // TODO calc crc of complete packet and send upstream 

            struct pico_ipv4_hdr *net_hdr = (struct pico_ipv4_hdr *) full->net_hdr;
            
            frag_dbg("[LUM:%s:%d] send the reassembled packet upstream \n",__FILE__,__LINE__);
            if(net_hdr)
            {
				net_hdr->crc = 0;
				net_hdr->crc = short_be(pico_checksum(net_hdr, full->net_len));
            }
            else
            {
				frag_dbg("[LUM:%s:%d] net_hdr NULL \n",__FILE__,__LINE__);
			}
				

#ifdef IPFRAG_DEBUG
            /*complete packet arrived: send full frame*/
            frag_dbg("[LUM:%s:%d] content of reassembled packet:  %p net_len:%d transport_len:%d\n",__FILE__,__LINE__,fragment->frame->buffer,fragment->frame->net_len,fragment->frame->transport_len);
            if(1)
            {
				int i;
				int s=full->net_len + 6;
				for(i=0;i < full->net_len + 6/*eth hdr*/;i=i+8)
				{
					frag_dbg("0x%04X: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X \n",i,
						full->buffer[i+0],full->buffer[i+1],full->buffer[i+2],full->buffer[i+3],
						full->buffer[i+4],full->buffer[i+5],full->buffer[i+6],full->buffer[i+7]);
				}
				frag_dbg("-----------------------------\n");
				for(i=0;i < full->transport_len;i=i+8)
				{
					frag_dbg("0x%04X: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X \n",i,
						full->buffer[s+i+0],full->buffer[s+i+1],full->buffer[s+i+2],full->buffer[s+i+3],
						full->buffer[s+i+4],full->buffer[s+i+5],full->buffer[s+i+6],full->buffer[s+i+7]);
				}
			}
#endif
            fragment->frame=NULL;

            pico_transport_receive(full, fragment->proto);
            full=NULL;
            
            // all done with this fragment: free it
            pico_fragment_free(fragment);
            fragment=NULL;
        }

    }
    return 0;
}



