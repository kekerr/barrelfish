/*
 * Copyright (c) 2014, University of Washington.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich. 
 * Attn: Systems Group.
 */

#include <stdio.h>
#include <assert.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/inthandler.h>
#include <barrelfish/sys_debug.h>
#include <skb/skb.h>
#include <sys/socket.h>
#include <netif/e1000.h>
#include <limits.h>
#include <barrelfish/waitset.h>
#include <barrelfish/waitset_chan.h>
#include <lwip/sock_chan_support.h>
#include <netdb.h>
#include <arranet.h>
#include <arranet_impl.h>
#include <acpi_client/acpi_client.h>
#include <arranet_debug.h>

static ether_terminate_queue ether_terminate_queue_ptr = NULL;
static ether_get_mac_address_t ether_get_mac_address_ptr = NULL;
static ether_transmit_pbuf_list_t ether_transmit_pbuf_list_ptr = NULL;
static ether_get_tx_free_slots tx_free_slots_fn_ptr = NULL;
static ether_handle_free_TX_slot handle_free_tx_slot_fn_ptr = NULL;
static ether_rx_register_buffer rx_register_buffer_fn_ptr = NULL;
static ether_rx_get_free_slots rx_get_free_slots_fn_ptr = NULL;

uint64_t interrupt_counter = 0;
uint64_t total_rx_p_count = 0;
uint64_t total_rx_datasize = 0;
struct client_closure *g_cl = NULL;

#define MAX_PACKETS     2000
#define PACKET_SIZE     2048
#define MAX_PEERS       256

static int use_vtd = 0;
static int vtd_coherency = 1;

struct peer {
    uint32_t ip;
    struct eth_addr mac;
};

// Configure static ARP entries here
// IP addresses are in network byte order!
static struct peer peers[MAX_PEERS] = {
    {
        // XXX: This needs to be updated each time the tap interface is re-initialized
        .ip = 0x0102000a,       // 10.0.2.1
        /* .mac.addr = "\x86\x86\x0b\xda\x22\xd7", */
        .mac.addr = "\x12\x67\xb9\x3e\xe2\x2c",
    },
    {
        // XXX: This needs to be updated each time the tap interface is re-initialized
        .ip = 0x0164a8c0,       // 192.168.100.1
        .mac.addr = "\x5e\x93\xf2\xf1\xeb\xfa",
    },
    {
        .ip = 0xaf06d080,       // 128.208.6.175 - swingout2
        .mac.addr = "\x90\xe2\xba\x3a\x2e\xdd",
    },
    {
        .ip = 0xec06d080,       // 128.208.6.236 - swingout3
        .mac.addr = "\xa0\x36\x9f\x0f\xfb\xe2",
    },
    {
        .ip = 0x8106d080,       // 128.208.6.129 - swingout4
        .mac.addr = "\xa0\x36\x9f\x10\x01\x6e",
    },
    {
        .ip = 0x8206d080,       // 128.208.6.130 - swingout5
        .mac.addr = "\xa0\x36\x9f\x10\x00\xa2",
    },
    {
        .ip = 0xc506d080,       // 128.208.6.197 - swingout6
        .mac.addr = "\xa0\x36\x9f\x10\x03\x52",
    },
};
static int peers_alloc = 7;             // Set number of static ARP here!

struct pkt_ip_headers {
    struct eth_hdr eth;
    struct ip_hdr ip;
} __attribute__ ((packed));

struct pkt_udp_headers {
    struct eth_hdr eth;
    struct ip_hdr ip;
    struct udp_hdr udp;
} __attribute__ ((packed));

struct pkt_tcp_headers {
    struct eth_hdr eth;
    struct ip_hdr ip;
    struct tcp_hdr tcp;
} __attribute__ ((packed));

static struct packet rx_packets[MAX_PACKETS];

/******** IP config *********/

struct mac2ip {
    uint8_t mac[ETHARP_HWADDR_LEN];
    uint32_t ip;
};

static struct mac2ip ip_config[] = {
    {   // QEMU
        .mac = "\x52\x54\x00\x12\x34\x56",
        /* .ip = 0x0a00020f,       // 10.0.2.15 */
        .ip = 0xc0a8640f,       // 192.168.100.15
    },
    {
        // QEMU2
        .mac = "\x52\x54\x00\x12\x34\x57",
        .ip = 0xc0a80102,       // 192.168.1.2
    },
    {   // swingout1 (and swingout1-vf0)
        .mac = "\xa0\x36\x9f\x10\x00\xa6",
        .ip = 0x80d00643,       // 128.208.6.67
    },
    {   // swingout1-vf1
        .mac = "\x22\xc9\xfc\x96\x83\xfc",
        .ip = 0x80d00644,       // 128.208.6.68
    },
    {   // swingout1-vf2
        .mac = "\xce\x43\x5b\xf7\x3e\x60",
        .ip = 0x80d00602,       // 128.208.6.2
    },
    {   // swingout1-vf3
        .mac = "\x6a\xb0\x62\xf6\xa7\x21",
        .ip = 0x80d00603,       // 128.208.6.3
    },
    {   // swingout1-vf4
        .mac = "\xb2\xdf\xf9\x39\xc6\x10",
        .ip = 0x80d00604,       // 128.208.6.4
    },
    {   // swingout1-vf5
        .mac = "\x92\x77\xe7\x3f\x80\x30",
        .ip = 0x80d0060c,       // 128.208.6.12
    },
    {   // swingout5
        .mac = "\xa0\x36\x9f\x10\x00\xa2",
        .ip = 0x80d00682,       // 128.208.6.130
    },
};

static uint8_t arranet_mymac[ETHARP_HWADDR_LEN];
static uint32_t arranet_myip = 0;

void ethernetif_backend_init(char *service_name, uint64_t queueid,
                             ether_get_mac_address_t get_mac_ptr,
                             ether_terminate_queue terminate_queue_ptr,
                             ether_transmit_pbuf_list_t transmit_ptr,
                             ether_get_tx_free_slots tx_free_slots_ptr,
                             ether_handle_free_TX_slot handle_free_tx_slot_ptr,
                             size_t rx_bufsz,
                             ether_rx_register_buffer rx_register_buffer_ptr,
                             ether_rx_get_free_slots rx_get_free_slots_ptr)
{
    ether_terminate_queue_ptr = terminate_queue_ptr;
    ether_get_mac_address_ptr = get_mac_ptr;
    ether_transmit_pbuf_list_ptr = transmit_ptr;
    tx_free_slots_fn_ptr = tx_free_slots_ptr;
    handle_free_tx_slot_fn_ptr = handle_free_tx_slot_ptr;
    rx_register_buffer_fn_ptr = rx_register_buffer_ptr;
    rx_get_free_slots_fn_ptr = rx_get_free_slots_ptr;
    /* printf("PBUF_POOL_BUFSIZE = %u, rx buffer size = %zu\n", PBUF_POOL_BUFSIZE, */
    /*        rx_bufsz); */
}

#define MAX_DRIVER_BUFS         16

static genpaddr_t rx_pbase = 0, tx_pbase = 0;
static genvaddr_t rx_vbase = 0, tx_vbase = 0;

static struct packet tx_packets[MAX_PACKETS];
/* static uint8_t tx_bufs[MAX_PACKETS][PACKET_SIZE]; */
static unsigned int tx_idx = 0;
/* static ssize_t tx_packets_available = MAX_PACKETS; */

#include <barrelfish/deferred.h>

static void packet_output(struct packet *p)
{
    struct driver_buffer bufs[MAX_DRIVER_BUFS];
    int n = 0;

    for (struct packet *q = p; q != NULL; q = q->next) {
        struct driver_buffer *buf = &bufs[n];

        /* if(q->payload < &tx_bufs[0][0] || q->payload >= &tx_bufs[MAX_PACKETS][PACKET_SIZE]) { */
        /*     printf("Called from %p %p\n", */
        /*            __builtin_return_address(0), */
        /*            __builtin_return_address(1)); */
        /*     assert(q->payload >= &tx_bufs[0][0] && q->payload < &tx_bufs[MAX_PACKETS][PACKET_SIZE]); */
        /* } */

        /* Send the data from the pbuf to the interface, one pbuf at a
           time. The size of the data in each pbuf is kept in the ->len
           variable. */
        assert(q->len > 0);

        // Check if it's from the RX region
        /* printf("RX region: Comparing %p against [%p:%p]\n", */
        /*        q->payload, */
        /*        (void *)rx_vbase, */
        /*        (void *)(rx_vbase + (MAX_PACKETS * PACKET_SIZE + 4096))); */
        if (!use_vtd) {
            if(((genvaddr_t)q->payload) >= rx_vbase &&
                    ((genvaddr_t)q->payload) < rx_vbase + (MAX_PACKETS * PACKET_SIZE + 4096)) {
                buf->pa = rx_pbase + ((genvaddr_t)q->payload - rx_vbase);
            } else if(((genvaddr_t)q->payload) >= tx_vbase &&
                    ((genvaddr_t)q->payload) < tx_vbase + (MAX_PACKETS * PACKET_SIZE)) {
                // It is from the TX region!
                buf->pa = tx_pbase + ((genvaddr_t)q->payload - tx_vbase);
            } else {
                // Check if it's in morecore's region
                struct morecore_state *mc_state = get_morecore_state();
                struct vspace_mmu_aware *mmu_state = &mc_state->mmu_state;
                genvaddr_t base = vregion_get_base_addr(&mmu_state->vregion);
                struct memobj_frame_list *i;

                // Walk frame list
                for(i = mmu_state->memobj.frame_list; i != NULL; i = i->next) {
                    // If address is completely within frame, we can resolve
                    // XXX: Everything else would be easier with an IOMMU
                    /* printf("Heap: Comparing [%p:%p] against [%p:%p]\n", */
                    /*        q->payload, q->payload + q->len, */
                    /*        (void *)(base + i->offset), */
                    /*        (void *)(base + i->offset + i->size)); */
                    if(base + i->offset <= (genvaddr_t)q->payload &&
                            ((genvaddr_t)q->payload) + q->len < base + i->offset + i->size) {
                        assert(i->pa != 0);

                        /* buf->pa = id.base + ((genvaddr_t)q->payload - base - i->offset); */
                        buf->pa = i->pa + ((genvaddr_t)q->payload - base - i->offset);
                        break;
                    }
                }

                if(i == NULL) {
                    // Check if it's in text/data region
                    int entry;
                    for(entry = 0; entry < mc_state->v2p_entries; entry++) {
                        struct v2pmap *pmap = &mc_state->v2p_mappings[entry];

                        // If address is completely within frame, we can resolve
                        // XXX: Everything else would be easier with an IOMMU
                        /* printf("BSS: Comparing [%p:%p] against [%p:%p]\n", */
                        /*        q->payload, q->payload + q->len, */
                        /*        (void *)(pmap->va), */
                        /*        (void *)(pmap->va + pmap->size)); */
                        if(pmap->va <= (genvaddr_t)q->payload &&
                                ((genvaddr_t)q->payload) + q->len < pmap->va + pmap->size) {
                            buf->pa = pmap->pa + ((genvaddr_t)q->payload - pmap->va);
                            break;
                        }

                    }

                    if(entry == mc_state->v2p_entries) {
                        printf("Called from %p %p %p\n",
                                __builtin_return_address(0),
                                __builtin_return_address(1),
                                __builtin_return_address(2));

                        USER_PANIC("Invalid pbuf! payload = %p, pa = %p, subpacket = %d\n",
                                q->payload, buf->pa, n);
                    }
                }
            }
        }
        /* printf("Sending: '%s'\n", (char *)q->payload); */

        buf->va = q->payload;
        buf->len = q->len;
#ifndef SENDMSG_WITH_COPY
        buf->opaque = q->opaque;
#else
        buf->opaque = q;
#endif
        buf->flags = q->flags;

        n++;
    }

    errval_t err = ether_transmit_pbuf_list_ptr(bufs, n);
    assert(err_is_ok(err));
}

static struct pkt_ip_headers packet_ip_header;
static struct pkt_udp_headers packet_udp_header;
static struct pkt_tcp_headers packet_tcp_header;

static struct peer *peers_get_from_ip(uint32_t ip)
{
    for(int i = 0; i < MAX_PEERS; i++) {
        if(ip == peers[i].ip) {
            return &peers[i];
        }
    }

    /* printf("NOT FOUND: %x\n", ip); */

    return NULL;
}

static struct peer *peers_get_next_free(void)
{
    if(peers_alloc < MAX_PEERS) {
        return &peers[peers_alloc++];
    } else {
        return NULL;
    }
}

static struct packet *get_tx_packet(void)
{
    struct packet *p = &tx_packets[tx_idx];

    // Busy-wait until packet not in flight
    while(p->len != 0) {
        /* printf("Pipeline stalled! tx_packets_available = %zd\n", tx_packets_available); */
        handle_free_tx_slot_fn_ptr();
        /* if(!handle_free_tx_slot_fn_ptr()) { */
        /*     printf("No packets could be freed!\n"); */
        /* } */
    }

    /* tx_packets_available--; */

    tx_idx = (tx_idx + 1) % MAX_PACKETS;
    return p;
}

void process_received_packet(struct driver_rx_buffer *buffer, size_t count,
                             uint64_t flags)
{

    struct packet *p = buffer->opaque;
    assert(p != NULL);
    assert(count == 1);
    p->len = buffer->len;

    /* printf("Incoming packet\n"); */
    /* printf("Got %p from driver\n", p); */

    assert(p >= rx_packets && p < &rx_packets[MAX_PACKETS]);

    // Drop packets with invalid checksums
    if(flags & NETIF_RXFLAG_IPCHECKSUM) {
        if(!(flags & NETIF_RXFLAG_IPCHECKSUM_GOOD)) {
            goto out;
        }
    }

    if(flags & NETIF_RXFLAG_L4CHECKSUM) {
        if(!(flags & NETIF_RXFLAG_L4CHECKSUM_GOOD)) {
            goto out;
        }
    }

    struct eth_hdr *ethhdr = (struct eth_hdr *)p->payload;
    switch (htons(ethhdr->type)) {
    case ETHTYPE_ARP:
        {
            struct etharp_hdr *arphdr = (struct etharp_hdr *)(p->payload + SIZEOF_ETH_HDR);
            uint32_t dipaddr = (arphdr->dipaddr.addrw[1] << 16) | arphdr->dipaddr.addrw[0];

            /* printf("%d: ARP request, dip = %x\n", disp_get_core_id(), dipaddr); */

            if(htons(arphdr->opcode) == ARP_REQUEST &&
               dipaddr == arranet_myip) {
                // Send reply
                struct packet outp;
		// XXX: Static payload! Need to lock if multithreaded!
                static uint8_t payload[PACKET_SIZE];
                struct eth_hdr *myeth = (struct eth_hdr *)payload;
                struct etharp_hdr *myarp = (struct etharp_hdr *)(payload + SIZEOF_ETH_HDR);

                /* printf("%d: ARP request for us!\n", disp_get_core_id()); */

                // ETH header
                memcpy(&myeth->dest, &arphdr->shwaddr, ETHARP_HWADDR_LEN);
                memcpy(&myeth->src, arranet_mymac, ETHARP_HWADDR_LEN);
                myeth->type = htons(ETHTYPE_ARP);

                // ARP header
                myarp->hwtype = htons(1);
                myarp->proto = htons(ETHTYPE_IP);
                myarp->hwlen = 6;
                myarp->protolen = 4;
                myarp->opcode = htons(ARP_REPLY);
                memcpy(&myarp->shwaddr, arranet_mymac, ETHARP_HWADDR_LEN);
                memcpy(&myarp->sipaddr, &arphdr->dipaddr, sizeof(myarp->sipaddr));
                memcpy(&myarp->dhwaddr, &arphdr->shwaddr, ETHARP_HWADDR_LEN);
                memcpy(&myarp->dipaddr, &arphdr->sipaddr, sizeof(myarp->dipaddr));

                outp.payload = payload;
                outp.len = SIZEOF_ETHARP_PACKET;
                /* outp.len = p->len; */
                outp.next = NULL;
                outp.flags = 0;
                outp.opaque = NULL;

                packet_output(&outp);
		static int arp_count = 0;
		arp_count++;
		if(arp_count > 100) {
		  printf("High ARP count!\n");
		}
                while(!e1000n_queue_empty()) thread_yield();
            }
        }
        break;

    case ETHTYPE_IP:
        {
            struct ip_hdr *iphdr = (struct ip_hdr *)(p->payload + SIZEOF_ETH_HDR);

            printf("%d: Is an IP packet, type %x\n", disp_get_core_id(), IPH_PROTO(iphdr));

            // Has to be UDP or TCP
            if(IPH_PROTO(iphdr) != IP_PROTO_UDP && IPH_PROTO(iphdr) != IP_PROTO_TCP) {
                goto out;
            }

            // XXX: Filter for our IP
            if(iphdr->dest.addr != arranet_myip) {
                goto out;
            }

            if(IPH_PROTO(iphdr) == IP_PROTO_UDP) {
                struct udp_hdr *udphdr = (struct udp_hdr *)(p->payload + SIZEOF_ETH_HDR + (IPH_HL(iphdr) * 4));
                uint8_t *payload = p->payload + SIZEOF_ETH_HDR + (IPH_HL(iphdr) * 4) + sizeof(struct udp_hdr);

                printf("Got UDP packet, dest IP %x, dest port %u\n",
                       htonl(iphdr->dest.addr), htons(udphdr->dest));

                // XXX: Filter for UDP ports 1234
                // TODO: Done in hardware soon
                if(htons(udphdr->dest) != 1234) {
                    goto out;
                }

                printf("payload '%s'\n", payload);

                struct packet *outp = get_tx_packet();
                struct eth_hdr *myeth = (struct eth_hdr *)outp->payload;
                struct ip_hdr *myip = (struct ip_hdr *)(outp->payload + SIZEOF_ETH_HDR);
                struct udp_hdr *myudp = (struct udp_hdr *)(outp->payload + SIZEOF_ETH_HDR + (IPH_HL(iphdr) * 4));
                uint8_t *mypayload = outp->payload + SIZEOF_ETH_HDR + (IPH_HL(iphdr) * 4) + sizeof(struct udp_hdr);

                // ETH header
                memcpy(&myeth->dest, &ethhdr->src, ETHARP_HWADDR_LEN);
                memcpy(&myeth->src, &ethhdr->dest, ETHARP_HWADDR_LEN);
                myeth->type = htons(ETHTYPE_IP);

                // IP header
                memcpy(myip, iphdr, sizeof(struct ip_hdr));
                memcpy(&myip->src, &iphdr->dest, sizeof(ip_addr_p_t));
                memcpy(&myip->dest, &iphdr->src, sizeof(ip_addr_p_t));

                // UDP header
                memcpy(myudp, udphdr, sizeof(struct udp_hdr));
                myudp->src = udphdr->dest;
                myudp->dest = udphdr->src;

                // Payload
                memcpy(mypayload, payload, htons(udphdr->len) - 8);

                outp->len = p->len;
                outp->next = NULL;
                outp->flags = 0;
                packet_output(outp);
            }

            // ARP management
            if(peers_get_from_ip(iphdr->src.addr) == NULL) {
                struct peer *newpeer = peers_get_next_free();
                assert(p != NULL);
                newpeer->ip = iphdr->src.addr;
                memcpy(&newpeer->mac.addr, &ethhdr->src.addr, ETHARP_HWADDR_LEN);
            }
#if 0
            // Push packets up - signal channel
            assert(inpkt == NULL);
            inpkt = p;
            if (waitset_chan_is_registered(&recv_chanstate)) {
                errval_t err = waitset_chan_trigger(&recv_chanstate);
                assert(err_is_ok(err));
            }

            // Return here, packet is in flight to user-space
            return;
#endif
        }
        break;

    default:
        break;
    }

 out:
    {
        //now we have consumed the preregistered pbuf containing a received packet
        //which was processed in this function. Therefore we have to register a new
        //free buffer for receiving packets.
        errval_t err = rx_register_buffer_fn_ptr(p->pa, p->payload, p);
        assert(err_is_ok(err));
    }
}

bool handle_tx_done(void *opaque)
{
    struct packet *p = opaque;
    if(p >= tx_packets && p < &tx_packets[MAX_PACKETS]) {
        /* printf("Packet from TX ring, marking available\n"); */
        // Mark packet as available, if coming from TX packet array
        p->len = 0;
        /* tx_packets_available++; */
#ifndef SENDMSG_WITH_COPY
    } else {
        if(opaque != NULL && arranet_tx_done_callback != NULL) {
            /* printf("Packet from app, handing up\n"); */
            arranet_tx_done_callback(opaque);
            /* } else { */
        /*     if(opaque == NULL) { */
        /*         printf("NULL packet\n"); */
        /*     } */
    }
#endif
    }

    return true;
}

/* allocate a single frame, mapping it into our vspace with given attributes */
static void *alloc_map_frame(vregion_flags_t attr, size_t size, struct capref *retcap)
{
    struct capref frame;
    errval_t r;

    r = frame_alloc(&frame, size, NULL);
    assert(err_is_ok(r));
    void *va;
    r = vspace_map_one_frame_attr(&va, size, frame, attr,
                                  NULL, NULL);
    if (err_is_fail(r)) {
        DEBUG_ERR(r, "vspace_map_one_frame failed");
        return NULL;
    }

    if (retcap != NULL) {
        *retcap = frame;
    }

    return va;
}

static const char *eat_opts[] = {
    "function=", "interrupts=", "queue=", "msix=", "vf=", "device=", "bus=", "use_vtd=",
    NULL
};

int main(int argc, char *argv[])
{
    uint8_t mac[6];

    errval_t err = skb_client_connect();
    assert(err_is_ok(err));

    err = skb_execute_query("vtd_enabled(0,C), write(vtd_coherency(C)).");
    if (err_is_ok(err)) {
        use_vtd = 1;
        for(int i = 0; i < argc; i++) { 
            if(!strncmp(argv[i], "use_vtd=", strlen("use_vtd=") - 1)) {
                use_vtd = !!atol(argv[i] + strlen("use_vtd="));
                break;
            }
        }
        err = skb_read_output("vtd_coherency(%d)", &vtd_coherency);
        assert(err_is_ok(err));
    }

    if (use_vtd) {
        err = connect_to_acpi();
        assert(err_is_ok(err));
        err = vtd_create_domain(cap_vroot);
        assert(err_is_ok(err));
        err = vtd_domain_add_device(0, 13, 16, 1, cap_vroot);
        assert(err_is_ok(err));
    }

    e1000n_driver_init(argc, argv);

    ether_get_mac_address_ptr(mac);
    printf("Arranet MAC address %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    struct capref frame;
    uint8_t *ram_base = alloc_map_frame(VREGION_FLAGS_READ_WRITE,
                                        MAX_PACKETS * PACKET_SIZE + 4096, &frame);
    assert(ram_base != NULL);

    struct frame_identity id;
    err = frame_identify(frame, &id);
    assert(err_is_ok(err));

    rx_pbase = id.base;
    rx_vbase = (genvaddr_t)ram_base;

    // Add buffers to RX ring for packet reception
    for(int i = 0; i < MAX_PACKETS; i++) {
        struct packet *p = &rx_packets[i];

        // XXX: Use this for recvfrom_arranet to get alignment
        /* p->payload = ram_base + (i * PACKET_SIZE) + 6; */
        /* p->pa = id.base + (i * PACKET_SIZE) + 6; */
        p->payload = ram_base + (i * PACKET_SIZE);
        p->pa = id.base + (i * PACKET_SIZE);
        p->len = PACKET_SIZE;
        p->flags = 0;

        err = rx_register_buffer_fn_ptr(p->pa, p->payload, p);
        assert(err_is_ok(err));
    }

    // Allocate TX buffers (to have them all backed by one frame)
    uint8_t *tx_bufs = alloc_map_frame(VREGION_FLAGS_READ_WRITE,
                                       MAX_PACKETS * PACKET_SIZE, &frame);
    assert(tx_bufs != NULL);

    err = frame_identify(frame, &id);
    assert(err_is_ok(err));
    tx_pbase = id.base;
    tx_vbase = (genvaddr_t)tx_bufs;

    // Initialize TX packet descriptors
    for(int i = 0; i < MAX_PACKETS; i++) {
        /* tx_packets[i].payload = tx_bufs[i]; */
        tx_packets[i].payload = tx_bufs + (i * PACKET_SIZE);
    }

    if (!vtd_coherency) {// For the UDP echo server
        sys_debug_flush_cache();
    }

    // Determine my static IP address
    for(int i = 0; i < sizeof(ip_config) / sizeof(struct mac2ip); i++) {
        struct mac2ip *e = &ip_config[i];
        if(!memcmp(mac, e->mac, ETHARP_HWADDR_LEN)) {
            arranet_myip = htonl(e->ip);
            memcpy(arranet_mymac, e->mac, ETHARP_HWADDR_LEN);
            break;
        }
    }

    if(arranet_myip == 0) {
        USER_PANIC("Arranet: No static IP config for this MAC address!\n");
    }

    /***** Initialize IP/Ethernet packet header template *****/
    {
        struct pkt_ip_headers *p = &packet_ip_header;

        // Initialize Ethernet header
        memcpy(&p->eth.src, mac, ETHARP_HWADDR_LEN);
        p->eth.type = htons(ETHTYPE_IP);

        // Initialize IP header
        p->ip._v_hl = 69;
        p->ip._tos = 0;
        p->ip._id = htons(3);
        p->ip._offset = 0;
        p->ip._ttl = 0xff;
        p->ip._proto = 0;
        p->ip._chksum = 0;
        p->ip.src.addr = arranet_myip;
    }

    /***** Initialize UDP/IP/Ethernet packet header template *****/
    {
        struct pkt_udp_headers *p = &packet_udp_header;

        // Initialize Ethernet header
        memcpy(&p->eth.src, mac, ETHARP_HWADDR_LEN);
        p->eth.type = htons(ETHTYPE_IP);

        // Initialize IP header
        p->ip._v_hl = 69;
        p->ip._tos = 0;
        p->ip._id = htons(3);
        p->ip._offset = 0;
        p->ip._ttl = 0xff;
        p->ip._proto = IP_PROTO_UDP;
        p->ip._chksum = 0;
        p->ip.src.addr = arranet_myip;

        // Initialize UDP header
        p->udp.chksum = 0;
    }

    /***** Initialize TCP/IP/Ethernet packet header template *****/
    {
        struct pkt_tcp_headers *p = &packet_tcp_header;

        // Initialize Ethernet header
        memcpy(&p->eth.src, mac, ETHARP_HWADDR_LEN);
        p->eth.type = htons(ETHTYPE_IP);

        // Initialize IP header
        p->ip._v_hl = 69;
        p->ip._tos = 0;
        p->ip._id = htons(3);
        p->ip._offset = 0;
        p->ip._ttl = 0xff;
        p->ip._proto = IP_PROTO_TCP;
        p->ip._chksum = 0;
        p->ip.src.addr = arranet_myip;

        // Initialize TCP header
        p->tcp.chksum = 0;
        p->tcp.wnd = 65535;
    }

    /***** Eat driver-specific options *****/
    static char *new_argv[ARG_MAX];
    int new_argc = 0;
    for(int i = 0; i < argc; i++) {
        int j;

        for(j = 0; eat_opts[j] != NULL; j++) {
            if(!strncmp(argv[i], eat_opts[j], strlen(eat_opts[j]) - 1)) {
                // Option matches -- delete!
                break;
            }
        }

        if(eat_opts[j] == NULL) {
            // Option doesn't match -- keep!
            new_argv[new_argc++] = argv[i];
        }
    }

    argc = new_argc;
    argv = new_argv;

    printf("e10ktest initialized.\n");

    for(;;) {
        arranet_polling_loop();
        //        e1000n_polling_loop(get_default_waitset());
    }
}
