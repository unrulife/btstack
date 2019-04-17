/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "mesh_lower_transport.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/mesh/beacon.h"
#include "btstack_util.h"
#include "btstack_memory.h"
#include "mesh_peer.h"
#include "mesh_lower_transport.h"

static uint16_t primary_element_address;
static void (*higher_layer_handler)( mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu);

static void mesh_print_hex(const char * name, const uint8_t * data, uint16_t len){
    printf("%-20s ", name);
    printf_hexdump(data, len);
}
// static void mesh_print_x(const char * name, uint32_t value){
//     printf("%20s: 0x%x", name, (int) value);
// }

// utility

// Transport PDU Getter
uint16_t mesh_transport_nid(mesh_transport_pdu_t * transport_pdu){
    return transport_pdu->network_header[0] & 0x7f;
}
uint16_t mesh_transport_ctl(mesh_transport_pdu_t * transport_pdu){
    return transport_pdu->network_header[1] >> 7;
}
uint16_t mesh_transport_ttl(mesh_transport_pdu_t * transport_pdu){
    return transport_pdu->network_header[1] & 0x7f;
}
uint32_t mesh_transport_seq(mesh_transport_pdu_t * transport_pdu){
    return big_endian_read_24(transport_pdu->network_header, 2);
}
uint32_t mesh_transport_seq_zero(mesh_transport_pdu_t * transport_pdu){
    return transport_pdu->seq_zero;
}
uint16_t mesh_transport_src(mesh_transport_pdu_t * transport_pdu){
    return big_endian_read_16(transport_pdu->network_header, 5);
}
uint16_t mesh_transport_dst(mesh_transport_pdu_t * transport_pdu){
    return big_endian_read_16(transport_pdu->network_header, 7);
}
void mesh_transport_set_nid_ivi(mesh_transport_pdu_t * transport_pdu, uint8_t nid_ivi){
    transport_pdu->network_header[0] = nid_ivi;
}
void mesh_transport_set_ctl_ttl(mesh_transport_pdu_t * transport_pdu, uint8_t ctl_ttl){
    transport_pdu->network_header[1] = ctl_ttl;
}
void mesh_transport_set_seq(mesh_transport_pdu_t * transport_pdu, uint32_t seq){
    big_endian_store_24(transport_pdu->network_header, 2, seq);
}
void mesh_transport_set_src(mesh_transport_pdu_t * transport_pdu, uint16_t src){
    big_endian_store_16(transport_pdu->network_header, 5, src);
}
void mesh_transport_set_dest(mesh_transport_pdu_t * transport_pdu, uint16_t dest){
    big_endian_store_16(transport_pdu->network_header, 7, dest);
}

// lower transport

// prototypes

static void mesh_lower_transport_run(void);
static void mesh_lower_transport_abort_transmission(void);

// state
static int                    lower_transport_retry_count;

static uint32_t               lower_transport_seq;

// lower transport incoming
static btstack_linked_list_t  lower_transport_incoming;

// lower transport ougoing
static btstack_linked_list_t lower_transport_outgoing;

static mesh_transport_pdu_t * lower_transport_outgoing_pdu;
static mesh_network_pdu_t   * lower_transport_outgoing_segment;
static uint16_t               lower_transport_outgoing_seg_o;

static void mesh_lower_transport_process_unsegmented_control_message(mesh_network_pdu_t *network_pdu){
    uint8_t * lower_transport_pdu     = mesh_network_pdu_data(network_pdu);
    uint8_t  opcode = lower_transport_pdu[0];
    printf("Unsegmented Control message, outgoing message %p, opcode %x\n", lower_transport_outgoing_pdu, opcode);
    uint16_t seq_zero_pdu;
    uint16_t seq_zero_out;
    uint32_t block_ack;
    switch (opcode){
        case 0:
            if (lower_transport_outgoing_pdu == NULL) break;
            seq_zero_pdu = big_endian_read_16(lower_transport_pdu, 1) >> 2;
            seq_zero_out = mesh_transport_seq(lower_transport_outgoing_pdu) & 0x1fff;
            block_ack = big_endian_read_32(lower_transport_pdu, 3);
            printf("[+] Segment Acknowledgment message with seq_zero %06x, block_ack %08x - outgoing seq %06x, block_ack %08x\n",
                   seq_zero_pdu, block_ack, seq_zero_out, lower_transport_outgoing_pdu->block_ack);
            if (block_ack == 0){
                // If a Segment Acknowledgment message with the BlockAck field set to 0x00000000 is received,
                // then the Upper Transport PDU shall be immediately cancelled and the higher layers shall be notified that
                // the Upper Transport PDU has been cancelled.
                printf("[+] Block Ack == 0 => Abort\n");
                mesh_lower_transport_abort_transmission();
                break;
            }
            if (seq_zero_pdu != seq_zero_out){
                printf("[!] Seq Zero doesn't match\n");
                break;
            }
            lower_transport_outgoing_pdu->block_ack &= ~block_ack;
            printf("[+] Updated block_ack %08x\n", lower_transport_outgoing_pdu->block_ack);
            if (lower_transport_outgoing_pdu->block_ack == 0){
                printf("[+] Sent complete\n");
                mesh_lower_transport_abort_transmission();
            }
            mesh_network_message_processed_by_higher_layer(network_pdu);
            break;
        default:
            higher_layer_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, (mesh_pdu_t *) network_pdu);
            break;
    }
}

// ack / incomplete message

static void mesh_lower_transport_setup_segmented_acknowledge_message(uint8_t * data, uint8_t obo, uint16_t seq_zero, uint32_t block_ack){
    // printf("ACK Upper Transport, seq_zero %x\n", seq_zero);
    data[0] = 0;    // SEG = 0, Opcode = 0
    big_endian_store_16( data, 1, (obo << 15) | (seq_zero << 2) | 0);    // OBO, SeqZero, RFU
    big_endian_store_32( data, 3, block_ack);
    mesh_print_hex("ACK Upper Transport", data, 7);
}

static void mesh_lower_transport_send_ack(uint16_t netkey_index, uint8_t ttl, uint16_t dest, uint16_t seq_zero, uint32_t block_ack){
    // setup ack message
    uint8_t  ack_msg[7];
    mesh_lower_transport_setup_segmented_acknowledge_message(ack_msg, 0, seq_zero, block_ack);
    //
    // "3.4.5.2: The output filter of the interface connected to advertising or GATT bearers shall drop all messages with TTL value set to 1."
    // if (ttl <= 1) return 0;

    // TODO: check transport_pdu_len depending on ctl

    // lookup network by netkey_index
    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return;

    // allocate network_pdu
    mesh_network_pdu_t * network_pdu = mesh_network_pdu_get();
    if (!network_pdu) return;

    // setup network_pdu
    mesh_network_setup_pdu(network_pdu, netkey_index, network_key->nid, 1, ttl, mesh_lower_transport_next_seq(), primary_element_address, dest, ack_msg, sizeof(ack_msg));

    // send network_pdu
    mesh_network_send_pdu(network_pdu);
}

static void mesh_lower_transport_send_ack_for_transport_pdu(mesh_transport_pdu_t *transport_pdu){
    uint16_t seq_zero = mesh_transport_seq_zero(transport_pdu);
    uint8_t ttl = mesh_transport_ttl(transport_pdu);
    uint16_t dest = mesh_transport_src(transport_pdu);
    uint16_t netkey_index = transport_pdu->netkey_index;
    printf("mesh_transport_send_ack_for_transport_pdu %p with netkey_index %x, TTL = %u, SeqZero = %x, SRC = %x, DST = %x\n",
           transport_pdu, netkey_index, ttl, seq_zero, primary_element_address, dest);
    mesh_lower_transport_send_ack(netkey_index, ttl, dest, seq_zero, transport_pdu->block_ack);
}

static void mesh_lower_transport_send_ack_for_network_pdu(mesh_network_pdu_t *network_pdu, uint16_t seq_zero, uint32_t block_ack) {
    uint8_t ttl = mesh_network_ttl(network_pdu);
    uint16_t dest = mesh_network_src(network_pdu);
    uint16_t netkey_index = network_pdu->netkey_index;
    printf("mesh_transport_send_ack_for_network_pdu %p with netkey_index %x, TTL = %u, SeqZero = %x, SRC = %x, DST = %x\n",
           network_pdu, netkey_index, ttl, seq_zero, primary_element_address, dest);
    mesh_lower_transport_send_ack(netkey_index, ttl, dest, seq_zero, block_ack);
}

static void mesh_lower_transport_stop_acknowledgment_timer(mesh_transport_pdu_t *transport_pdu){
    if (!transport_pdu->acknowledgement_timer_active) return;
    transport_pdu->acknowledgement_timer_active = 0;
    btstack_run_loop_remove_timer(&transport_pdu->acknowledgement_timer);
}

static void mesh_lower_transport_stop_incomplete_timer(mesh_transport_pdu_t *transport_pdu){
    if (!transport_pdu->incomplete_timer_active) return;
    transport_pdu->incomplete_timer_active = 0;
    btstack_run_loop_remove_timer(&transport_pdu->incomplete_timer);
}

// stops timers and updates reassembly engine
static void mesh_lower_transport_segmented_message_complete(mesh_transport_pdu_t *transport_pdu){
    // set flag
    transport_pdu->message_complete = 1;
    // stop timers
    mesh_lower_transport_stop_acknowledgment_timer(transport_pdu);
    mesh_lower_transport_stop_incomplete_timer(transport_pdu);
    // stop reassembly
    mesh_peer_t * peer = mesh_peer_for_addr(mesh_transport_src(transport_pdu));
    if (peer){
        peer->transport_pdu = NULL;
    }
}

static void mesh_lower_transport_rx_ack_timeout(btstack_timer_source_t *ts){
    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) btstack_run_loop_get_timer_context(ts);
    printf("ACK: acknowledgement timer fired for %p, send ACK\n", transport_pdu);
    transport_pdu->acknowledgement_timer_active = 0;
    mesh_lower_transport_send_ack_for_transport_pdu(transport_pdu);
}

static void mesh_lower_transport_rx_incomplete_timeout(btstack_timer_source_t *ts){
    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) btstack_run_loop_get_timer_context(ts);
    printf("mesh_transport_rx_incomplete_timeout for %p - give up\n", transport_pdu);
    mesh_lower_transport_segmented_message_complete(transport_pdu);
    // free message
    btstack_memory_mesh_transport_pdu_free(transport_pdu);
}

static void mesh_lower_transport_start_acknowledgment_timer(mesh_transport_pdu_t *transport_pdu, uint32_t timeout,
                                                            void (*callback)(btstack_timer_source_t *ts)){
    printf("ACK: start ack timer for %p, timeout %u ms\n", transport_pdu, (int) timeout);
    btstack_run_loop_set_timer(&transport_pdu->acknowledgement_timer, timeout);
    btstack_run_loop_set_timer_handler(&transport_pdu->acknowledgement_timer, callback);
    btstack_run_loop_set_timer_context(&transport_pdu->acknowledgement_timer, transport_pdu);
    btstack_run_loop_add_timer(&transport_pdu->acknowledgement_timer);
    transport_pdu->acknowledgement_timer_active = 1;
}

static void mesh_lower_transport_restart_incomplete_timer(mesh_transport_pdu_t *transport_pdu, uint32_t timeout,
                                                          void (*callback)(btstack_timer_source_t *ts)){
    printf("RX-(re)start incomplete timer for %p, timeout %u ms\n", transport_pdu, (int) timeout);
    if (transport_pdu->incomplete_timer_active){
        btstack_run_loop_remove_timer(&transport_pdu->incomplete_timer);
    }
    btstack_run_loop_set_timer(&transport_pdu->incomplete_timer, timeout);
    btstack_run_loop_set_timer_handler(&transport_pdu->incomplete_timer, callback);
    btstack_run_loop_set_timer_context(&transport_pdu->incomplete_timer, transport_pdu);
    btstack_run_loop_add_timer(&transport_pdu->incomplete_timer);
    transport_pdu->incomplete_timer_active = 1;
}

// abort outgoing transmission
static void mesh_lower_transport_abort_transmission(void){
    // stop ack timers
    mesh_lower_transport_stop_acknowledgment_timer(lower_transport_outgoing_pdu);

    // notify upper transport
    mesh_transport_pdu_t * pdu   = lower_transport_outgoing_pdu;
    lower_transport_outgoing_pdu = NULL;
    higher_layer_handler(MESH_TRANSPORT_PDU_SENT, MESH_TRANSPORT_STATUS_SEND_ABORT_BY_REMOTE, (mesh_pdu_t *) pdu);
}

static mesh_transport_pdu_t * mesh_lower_transport_pdu_for_segmented_message(mesh_network_pdu_t *network_pdu){
    uint16_t src = mesh_network_src(network_pdu);
    uint16_t seq_zero = ( big_endian_read_16(mesh_network_pdu_data(network_pdu), 1) >> 2) & 0x1fff;
    printf("mesh_transport_pdu_for_segmented_message: seq_zero %x\n", seq_zero);
    mesh_peer_t * peer = mesh_peer_for_addr(src);
    if (!peer) {
        return NULL;
    }
    printf("mesh_seq_zero_validate(%x, %x) -- last (%x, %x)\n", src, seq_zero, peer->address, peer->seq_zero);

    // reception of transport message ongoing
    if (peer->transport_pdu){
        // check if segment for same seq zero
        uint16_t active_seq_zero = mesh_transport_seq_zero(peer->transport_pdu);
        if (active_seq_zero == seq_zero) {
            printf("mesh_transport_pdu_for_segmented_message: segment for current transport pdu with SeqZero %x\n", active_seq_zero);
            return peer->transport_pdu;
        } else {
            // seq zero differs from current transport pdu, but current pdu is not complete
            printf("mesh_transport_pdu_for_segmented_message: drop segment. current transport pdu SeqZero %x, now %x\n", active_seq_zero, seq_zero);
            return NULL;
        }
    }

    // send ACK if segment for previously completed transport pdu (no ongoing reception, block ack is cleared)
    if ((seq_zero == peer->seq_zero) && (peer->block_ack != 0)){
        printf("mesh_transport_pdu_for_segmented_message: segment for last completed message. send ack\n");
        mesh_lower_transport_send_ack_for_network_pdu(network_pdu, seq_zero, peer->block_ack);
        return NULL;
    }

    // reconstruct lowest 24 bit of SeqAuth
    uint32_t seq = mesh_network_seq(network_pdu);
    uint32_t seq_auth = (seq & 0xffe000) | seq_zero;
    if (seq_auth > seq){
        seq_auth -= 0x2000;
    }

    // no transport pdu active, check if seq zero is new
    if (seq_auth > peer->seq_auth){
        mesh_transport_pdu_t * pdu = mesh_transport_pdu_get();
        if (!pdu) return NULL;

        // cache network pdu header
        memcpy(pdu->network_header, network_pdu->data, 9);
        // store lower 24 bit of SeqAuth for App / Device Nonce
        big_endian_store_24(pdu->network_header, 2, seq_auth);

        // store meta data in new pdu
        pdu->netkey_index = network_pdu->netkey_index;
        pdu->block_ack = 0;
        pdu->acknowledgement_timer_active = 0;
        pdu->message_complete = 0;
        pdu->seq_zero = seq_zero;

        // update peer info
        peer->transport_pdu = pdu;
        peer->seq_zero      = seq_zero;
        peer->seq_auth      = seq_auth;
        peer->block_ack     = 0;

        printf("mesh_transport_pdu_for_segmented_message: setup transport pdu %p for src %x, seq %06x, seq_zero %x\n", pdu, src, mesh_transport_seq(pdu), seq_zero);

        return peer->transport_pdu;
    }  else {
        // seq zero differs from current transport pdu
        printf("mesh_transport_pdu_for_segmented_message: drop segment for old seq %x\n", seq_zero);
        return NULL;
    }
}

static void mesh_lower_transport_process_segment( mesh_transport_pdu_t * transport_pdu, mesh_network_pdu_t * network_pdu){

    uint8_t * lower_transport_pdu     = mesh_network_pdu_data(network_pdu);
    uint8_t   lower_transport_pdu_len = mesh_network_pdu_len(network_pdu);

    // get akf_aid & transmic
    transport_pdu->akf_aid      = lower_transport_pdu[0];
    transport_pdu->transmic_len = lower_transport_pdu[1] & 0x80 ? 8 : 4;

    // get seq_zero
    uint16_t seq_zero =  ( big_endian_read_16(lower_transport_pdu, 1) >> 2) & 0x1fff;

    // get seg fields
    uint8_t  seg_o    =  ( big_endian_read_16(lower_transport_pdu, 2) >> 5) & 0x001f;
    uint8_t  seg_n    =  lower_transport_pdu[3] & 0x1f;
    uint8_t   segment_len  =  lower_transport_pdu_len - 4;
    uint8_t * segment_data = &lower_transport_pdu[4];

    printf("mesh_lower_transport_process_segment: seq zero %04x, seg_o %02x, seg_n %02x, transmic len: %u\n", seq_zero, seg_o, seg_n, transport_pdu->transmic_len * 8);
    mesh_print_hex("Segment", segment_data, segment_len);

    // store segment
    memcpy(&transport_pdu->data[seg_o * 12], segment_data, 12);
    // mark as received
    transport_pdu->block_ack |= (1<<seg_o);
    // last segment -> store len
    if (seg_o == seg_n){
        transport_pdu->len = (seg_n * 12) + segment_len;
        printf("Assembled payload len %u\n", transport_pdu->len);
    }

    // check for complete
    int i;
    for (i=0;i<=seg_n;i++){
        if ( (transport_pdu->block_ack & (1<<i)) == 0) return;
    }

    mesh_print_hex("Assembled payload", transport_pdu->data, transport_pdu->len);

    // mark as done
    mesh_lower_transport_segmented_message_complete(transport_pdu);

    // store block ack in peer info
    mesh_peer_t * peer = mesh_peer_for_addr(mesh_transport_src(transport_pdu));
    // TODO: check if NULL check can be removed
    if (peer){
        peer->block_ack = transport_pdu->block_ack;
    }

    // send ack
    mesh_lower_transport_send_ack_for_transport_pdu(transport_pdu);

    // forward to upper transport
    higher_layer_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, (mesh_pdu_t*) transport_pdu);
}

void mesh_lower_transport_message_processed_by_higher_layer(mesh_pdu_t * pdu){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_NETWORK:
            mesh_network_message_processed_by_higher_layer((mesh_network_pdu_t *) pdu);
            break;
        case MESH_PDU_TYPE_TRANSPORT:
            mesh_transport_pdu_free((mesh_transport_pdu_t *) pdu);
            break;
        default:
            break;
    }
}

static void mesh_lower_transport_network_pdu_sent(mesh_network_pdu_t *network_pdu);

static void mesh_lower_transport_tx_ack_timeout(btstack_timer_source_t * ts);

void mesh_lower_transport_received_message(mesh_network_callback_type_t callback_type, mesh_network_pdu_t *network_pdu){
    mesh_peer_t * peer;
    uint16_t src;
    uint16_t seq;
    switch (callback_type){
        case MESH_NETWORK_PDU_RECEIVED:
            src = mesh_network_src(network_pdu);
            seq = mesh_network_seq(network_pdu);
            peer = mesh_peer_for_addr(src);
            printf("Transport: received message. SRC %x, SEQ %x\n", src, seq);
            // validate seq
            if (peer && seq > peer->seq){
                // track seq
                peer->seq = seq;
                // add to list and go
                btstack_linked_list_add_tail(&lower_transport_incoming, (btstack_linked_item_t *) network_pdu);
                mesh_lower_transport_run();
            } else {
                // drop packet
                printf("Transport: drop packet - src/seq auth failed\n");
                mesh_network_message_processed_by_higher_layer(network_pdu);
            }
            break;
        case MESH_NETWORK_PDU_SENT:
            mesh_lower_transport_network_pdu_sent(network_pdu);
            break;
        default:
            break;
    }
}

static void mesh_lower_transport_setup_segment(mesh_transport_pdu_t *transport_pdu, uint8_t seg_o, mesh_network_pdu_t *network_pdu){

    int ctl = mesh_transport_ctl(transport_pdu);
    uint16_t max_segment_len = ctl ? 8 : 12;    // control 8 bytes (64 bit NetMic), access 12 bytes (32 bit NetMIC)

    uint32_t seq      = mesh_lower_transport_next_seq();
    uint16_t seq_zero = mesh_transport_seq(transport_pdu) & 0x01fff;
    uint8_t  seg_n    = (transport_pdu->len - 1) / max_segment_len;
    uint8_t  szmic    = ((!ctl) && (transport_pdu->transmic_len == 8)) ? 1 : 0; // only 1 for access messages with 64 bit TransMIC
    uint8_t  nid      = mesh_transport_nid(transport_pdu);
    uint8_t  ttl      = mesh_transport_ttl(transport_pdu);
    uint16_t src      = mesh_transport_src(transport_pdu);
    uint16_t dest     = mesh_transport_dst(transport_pdu);

    // current segment.
    uint16_t seg_offset = seg_o * max_segment_len;

    uint8_t lower_transport_pdu_data[16];
    lower_transport_pdu_data[0] = 0x80 |  transport_pdu->akf_aid;
    big_endian_store_24(lower_transport_pdu_data, 1, (szmic << 23) | (seq_zero << 10) | (seg_o << 5) | seg_n);
    uint16_t segment_len = btstack_min(transport_pdu->len - seg_offset, max_segment_len);
    memcpy(&lower_transport_pdu_data[4], &transport_pdu->data[seg_offset], segment_len);
    uint16_t lower_transport_pdu_len = 4 + segment_len;

    mesh_network_setup_pdu(network_pdu, transport_pdu->netkey_index, nid, 0, ttl, seq, src, dest, lower_transport_pdu_data, lower_transport_pdu_len);
}

static void mesh_lower_transport_send_next_segment(void){
    if (!lower_transport_outgoing_pdu) return;

    int ctl = mesh_transport_ctl(lower_transport_outgoing_pdu);
    uint16_t max_segment_len = ctl ? 8 : 12;    // control 8 bytes (64 bit NetMic), access 12 bytes (32 bit NetMIC)
    uint8_t  seg_n = (lower_transport_outgoing_pdu->len - 1) / max_segment_len;

    // find next unacknowledged segement
    while ((lower_transport_outgoing_seg_o <= seg_n) && ((lower_transport_outgoing_pdu->block_ack & (1 << lower_transport_outgoing_seg_o)) == 0)){
        lower_transport_outgoing_seg_o++;
    }

    if (lower_transport_outgoing_seg_o > seg_n){
        printf("[+] Upper transport, send segmented pdu complete (dst %x)\n", mesh_transport_dst(lower_transport_outgoing_pdu));
        lower_transport_outgoing_seg_o   = 0;

        // done for unicast, ack timer already set, too
        if (mesh_network_address_unicast(mesh_transport_dst(lower_transport_outgoing_pdu))) return;

        // done, more?
        if (lower_transport_retry_count == 0){
            printf("[+] Upper transport, message unacknowledged -> free\n");
            // notify upper transport
            mesh_transport_pdu_t * pdu   = lower_transport_outgoing_pdu;
            lower_transport_outgoing_pdu = NULL;
            higher_layer_handler(MESH_TRANSPORT_PDU_SENT, MESH_TRANSPORT_STATUS_SEND_FAILED, (mesh_pdu_t *) pdu);
            return;
        }

        // start retry
        printf("[+] Upper transport, message unacknowledged retry count %u\n", lower_transport_retry_count);
        lower_transport_retry_count--;
    }

    if (mesh_network_address_unicast(mesh_transport_dst(lower_transport_outgoing_pdu))){
        // restart acknowledgment timer for unicast dst
        // - "This timer shall be set to a minimum of 200 + 50 * TTL milliseconds."
        if (lower_transport_outgoing_pdu->acknowledgement_timer_active){
            btstack_run_loop_remove_timer(&lower_transport_outgoing_pdu->incomplete_timer);
            lower_transport_outgoing_pdu->acknowledgement_timer_active = 0;
        }
        uint32_t timeout = 200 + 50 * mesh_transport_ttl(lower_transport_outgoing_pdu);
        mesh_lower_transport_start_acknowledgment_timer(lower_transport_outgoing_pdu, timeout,
                                                        &mesh_lower_transport_tx_ack_timeout);
    }

    mesh_lower_transport_setup_segment(lower_transport_outgoing_pdu, lower_transport_outgoing_seg_o,
                                       lower_transport_outgoing_segment);

    printf("[+] Upper transport, send segmented pdu: seg_o %x, seg_n %x\n", lower_transport_outgoing_seg_o, seg_n);
    mesh_print_hex("LowerTransportPDU", lower_transport_outgoing_segment->data, lower_transport_outgoing_segment->len);

    // next segment
    lower_transport_outgoing_seg_o++;

    // send network pdu
    mesh_network_send_pdu(lower_transport_outgoing_segment);
}

static void mesh_lower_transport_network_pdu_sent(mesh_network_pdu_t *network_pdu){
    // figure out what pdu was sent

    // single segment of segmented message?
    if (lower_transport_outgoing_segment == network_pdu){
        mesh_lower_transport_send_next_segment();
        return;
    }

    // Segment Acknowledgment message sent by us?
    if (mesh_network_control(network_pdu) && network_pdu->data[0] == 0){
        btstack_memory_mesh_network_pdu_free(network_pdu);
        return;
    }

    // other
    higher_layer_handler(MESH_TRANSPORT_PDU_SENT, MESH_TRANSPORT_STATUS_SUCCESS, (mesh_pdu_t *) network_pdu);
}

static void mesh_lower_transport_send_segmented_pdu_once(mesh_transport_pdu_t *transport_pdu){

    if (lower_transport_retry_count == 0){
        printf("[!] Upper transport, send segmented pdu failed, retries exhausted\n");
        return;
    }

    // chop into chunks
    printf("[+] Upper transport, send segmented pdu (retry count %u)\n", lower_transport_retry_count);
    lower_transport_retry_count--;

    // setup
    lower_transport_outgoing_pdu     = transport_pdu;
    lower_transport_outgoing_seg_o   = 0;

    // setup block ack - set bit for segment to send, clear on ack
    int      ctl = mesh_transport_ctl(lower_transport_outgoing_pdu);
    uint16_t max_segment_len = ctl ? 8 : 12;    // control 8 bytes (64 bit NetMic), access 12 bytes (32 bit NetMIC)
    uint8_t  seg_n = (lower_transport_outgoing_pdu->len - 1) / max_segment_len;
    if (seg_n < 31){
        transport_pdu->block_ack = (1 << (seg_n+1)) - 1;
    } else {
        transport_pdu->block_ack = 0xffffffff;
    }

    // start sending
    mesh_lower_transport_send_next_segment();
}

void mesh_lower_transport_send_segmented_pdu(mesh_transport_pdu_t *transport_pdu){
    // queue transport (= segmented pdu)
    btstack_linked_list_add_tail(&lower_transport_outgoing, (btstack_linked_item_t*) transport_pdu);
    mesh_lower_transport_run();
}

void mesh_lower_transport_send_unsegmented_pdu(mesh_network_pdu_t *network_pdu){
    btstack_linked_list_add_tail(&lower_transport_outgoing, (btstack_linked_item_t*) network_pdu);
    mesh_lower_transport_run();
}

static void mesh_lower_transport_tx_ack_timeout(btstack_timer_source_t * ts){
    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) btstack_run_loop_get_timer_context(ts);
    printf("[+] Lower transport, acknowledgement timer fired for %p\n", transport_pdu);
    transport_pdu->acknowledgement_timer_active = 0;
    // send remaining segments again
    mesh_lower_transport_send_segmented_pdu_once(transport_pdu);
}

static void mesh_lower_transport_run(void){
    while(!btstack_linked_list_empty(&lower_transport_incoming)){
        // get next message
        mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) btstack_linked_list_pop(&lower_transport_incoming);
        // segmented?
        if (mesh_network_segmented(network_pdu)){
            mesh_transport_pdu_t * transport_pdu = mesh_lower_transport_pdu_for_segmented_message(network_pdu);
            if (!transport_pdu) return;
            // start acknowledgment timer if inactive
            if (transport_pdu->acknowledgement_timer_active == 0){
                // - "The acknowledgment timer shall be set to a minimum of 150 + 50 * TTL milliseconds"
                uint32_t timeout = 150 + 50 * mesh_network_ttl(network_pdu);
                mesh_lower_transport_start_acknowledgment_timer(transport_pdu, timeout,
                                                                &mesh_lower_transport_rx_ack_timeout);
            }
            // restart incomplete timer
            mesh_lower_transport_restart_incomplete_timer(transport_pdu, 10000,
                                                          &mesh_lower_transport_rx_incomplete_timeout);
            mesh_lower_transport_process_segment(transport_pdu, network_pdu);
            mesh_network_message_processed_by_higher_layer(network_pdu);
        } else {
            // control?
            if (mesh_network_control(network_pdu)){
                // unsegmented control message (not encrypted)
                mesh_lower_transport_process_unsegmented_control_message(network_pdu);
            } else {
                // unsegmented access message (encrypted)
                higher_layer_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, (mesh_pdu_t *) network_pdu);
            }
        }
    }

    // check if outgoing segmented pdu is active
    if (lower_transport_outgoing_pdu) return;

    while(!btstack_linked_list_empty(&lower_transport_outgoing)) {
        // get next message
        mesh_transport_pdu_t * transport_pdu;
        mesh_network_pdu_t   * network_pdu;
        mesh_pdu_t * pdu = (mesh_pdu_t *) btstack_linked_list_pop(&lower_transport_outgoing);
        switch (pdu->pdu_type) {
            case MESH_PDU_TYPE_NETWORK:
                network_pdu = (mesh_network_pdu_t *) pdu;
                mesh_network_send_pdu(network_pdu);
                break;
            case MESH_PDU_TYPE_TRANSPORT:
                transport_pdu = (mesh_transport_pdu_t *) pdu;
                // start sending segmented pdu
                lower_transport_retry_count = 2;
                mesh_lower_transport_send_segmented_pdu_once(transport_pdu);
                break;
            default:
                break;
        }
    }
}

uint32_t mesh_lower_transport_next_seq(void){
    return lower_transport_seq++;
}

uint32_t mesh_lower_transport_peek_seq(void){
    return lower_transport_seq;
}

void mesh_lower_transport_set_seq(uint32_t seq){
    lower_transport_seq = seq;
}

static void mesh_lower_transport_dump_network_pdus(const char *name, btstack_linked_list_t *list){
    printf("List: %s:\n", name);
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, list);
    while (btstack_linked_list_iterator_has_next(&it)){
        mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t*) btstack_linked_list_iterator_next(&it);
        printf("- %p: ", network_pdu); printf_hexdump(network_pdu->data, network_pdu->len);
    }
}
static void mesh_lower_transport_reset_network_pdus(btstack_linked_list_t *list){
    while (!btstack_linked_list_empty(list)){
        mesh_network_pdu_t * pdu = (mesh_network_pdu_t *) btstack_linked_list_pop(list);
        btstack_memory_mesh_network_pdu_free(pdu);
    }
}

void mesh_lower_transport_dump(void){
    // static btstack_linked_list_t upper_transport_control;
    // static btstack_linked_list_t upper_transport_access;
    mesh_lower_transport_dump_network_pdus("lower_transport_incoming", &lower_transport_incoming);
}

void mesh_lower_transport_reset(void){
    // static btstack_linked_list_t upper_transport_control;
    // static btstack_linked_list_t upper_transport_access;
    mesh_lower_transport_reset_network_pdus(&lower_transport_incoming);
    if (lower_transport_outgoing_pdu){
        mesh_transport_pdu_free(lower_transport_outgoing_pdu);
        lower_transport_outgoing_pdu = NULL;
    }
}

void mesh_lower_transport_init(){
    // register with network layer
    mesh_network_set_higher_layer_handler(&mesh_lower_transport_received_message);
    // allocate network_pdu for segmentation
    lower_transport_outgoing_segment = mesh_network_pdu_get();
}

void mesh_lower_transport_set_primary_element_address(uint16_t unicast_address){
    primary_element_address = unicast_address;
}

void mesh_lower_transport_set_higher_layer_handler(void (*pdu_handler)( mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu)){
    higher_layer_handler = pdu_handler;
}
