#include <NRF24Zigbee.h>

#ifdef USE_PRINTF

int serial_putc( char c, struct __file * )
{
  Serial.write( c );
  return c;
}
void printf_begin(void)
{
  fdevopen( &serial_putc, 0 );
}

#else

void printf_begin(void)
{
}

#endif

void print_info()
{
  debug_printf("\n############### REG TRACE START ###############\n");
  debug_printf("EN_AA     = 0x%02X\n", read_register(EN_AA));
  debug_printf("EN_RXADDR = 0x%02X\n", read_register(EN_RXADDR));
  debug_printf("RX_ADDR_P2= '%c' RX_PW_P2=%u\n", read_register(RX_ADDR_P2), read_register(RX_PW_P2));
  debug_printf("RX_ADDR_P3= '%c' RX_PW_P3=%u\n", read_register(RX_ADDR_P3), read_register(RX_PW_P3));
  debug_printf("RX_ADDR_P4= '%c' RX_PW_P4=%u\n", read_register(RX_ADDR_P4), read_register(RX_PW_P4));
  debug_printf("###############  REG TRACE END  ###############\n\n");
}

static rx_node_handle rx_fifo_mem[MAX_FIFO_SIZE];/* This consumes 528 bytes */
static rx_fifo_handle fifo_instance;


void nrf_set_broadcast_addr(uint8_t addr)
{
  config_register(RX_ADDR_P2, addr);
}

bool phy_layer_data_ready(void)
{
  return nrf_data_ready();
}

//rx_node_handle phy_rx_node_instance;

void phy_packet_trace(phy_packet_handle * packet, uint8_t mode = 0)
{
  if (mode == 0) {
    pr_debug("######## Phy packet trace ########\n");
    pr_debug("type        :%4u\n", packet->type);
    pr_debug("length      :%4u\n", packet->length);
    pr_debug("packet_index:%4u\n", packet->packet_index);
    pr_debug("slice_size  :%4u\n", packet->slice_size);
    pr_debug("slice_index : %4u\n", packet->slice_index);
    pr_debug("src_addr    :0x%04X\n", *(uint16_t *)packet->src_addr);
    pr_debug("crc         :0x%02X\n", packet->crc);
    pr_debug("data        :");
    for (int i = 0; i < packet->length; i ++) {
      if (i % 10 == 0)
        debug_printf("\n");
      debug_printf("0x%02X ", packet->data[i]);
    }  
  }
  else {
    uint8_t *ptr = (uint8_t *)packet;
    pr_info("######## Phy packet trace ########\n");
    for (int i = 0; i < 32; i ++) {
      if (i % 10 == 0)
        debug_printf("\n");
      debug_printf("0x%02X ", ptr[i]);
    }
  }
  debug_printf("\n\n");
}

inline void phy_layer_reset_node(rx_node_handle *node)
{
  node->node_status = NODE_INVALID;
  node->recv_chain = 0x00;
}

bool phy_layer_send_slice_packet(phy_packet_handle * packet, uint32_t max_retry = 100)
{
  nrf_reliable_send((uint8_t *)packet, 32, max_retry);
}

void phy_layer_listener(void)
{
  static uint8_t raw_data[32];
  static phy_packet_handle * packet = (phy_packet_handle *)raw_data;
  static rx_node_handle * phy_rx_node = NULL;
  static uint8_t in_receive_state = 0;
  static uint16_t packet_receive_duration = 0;
  static uint16_t packet_max_duration = 0;
  
  static uint8_t last_slice_index = 0;
  static uint16_t last_src_addr = 0;
  static uint8_t last_packet_index = 0;

  /* Listener shall check if got any rx data from phy */

  /* TODO: Add timeout check */
  if (in_receive_state && packet_receive_duration > packet_max_duration) {
    pr_err("Time out for this message chain reception.\n");
    /* Do something for timeout handle */
  }

  if (phy_layer_data_ready()) {
    uint8_t status = read_register(STATUS);
    uint8_t pipe = (status >> 1) & 0x07; /* Get what pipe channel is
                                         this data from */
    nrf_get_data(raw_data);
    //phy_packet_trace(packet, 0);

    if (crc_calculate((uint8_t *)packet, PHY_PACKET_HEADER_SIZE) != packet->crc) {
        pr_err("Recv packet crc check err, raw=0x%02X calc=0x%02X, drop it!\n", packet->crc, crc_calculate((uint8_t *)packet, 2));
        //phy_packet_trace(packet, 1);
        return ;
    }

    if (packet->slice_index == last_slice_index && packet->src_addr == last_src_addr
          && packet->packet_index == last_packet_index) {
      pr_err("Repeat packs\n");
      goto exit; /* We omit this repeat packet */
    }

    if (packet->type == MESSAGE_PACKET) {
      /* Check if this is a repeat pack */

      phy_rx_node = fifo_find_node(&fifo_instance, packet->src_addr, packet->packet_index);

      if (!phy_rx_node) {
        rx_node_handle tmp_fifo_node;
        SRC_ADDR_COPY(tmp_fifo_node.src_addr, packet->src_addr);
        tmp_fifo_node.packet_index = packet->packet_index;
        /* Create a new node which doesnt exsits before */
        //pr_debug("packet->src_addr = 0x%04X\n", *(uint16_t *)packet->src_addr);
        //pr_debug("Push new node into fifo, src_addr=0x%04X, packet_index=0x%02X\n", *(uint16_t *)tmp_fifo_node.src_addr, tmp_fifo_node.packet_index);
        fifo_in(&fifo_instance, &tmp_fifo_node);        
        phy_rx_node = fifo_find_node(&fifo_instance, packet->src_addr, packet->packet_index);
        phy_layer_reset_node(phy_rx_node);
      }

      if (packet->slice_index == 0) { /* This is the first slice */
          pr_debug("First %u/%u Mac:0x%04X Index:%u\n", packet->slice_index, 
            packet->slice_size - 1, *(uint16_t *)packet->src_addr, packet->packet_index);
          phy_layer_reset_node(phy_rx_node);
      }
      /* If this is the last slice or timeout , we shall send 
                                              control msg to sender */
      /* Don't use 'else' ,cause slice_size could be 1 */
      if (packet->slice_index == packet->slice_size-1) { 
          phy_rx_node->recv_chain |= (1 << (uint8_t)packet->slice_index);
          pr_debug("Last  %u/%u Mac:0x%04X Index:%u\n", packet->slice_index, 
            packet->slice_size - 1, *(uint16_t *)packet->src_addr, packet->packet_index);
            /* Check if all slices received and send ack to sender .
             If not all slices received ,we have two ways to handle this 
             situation.One is just drop all slices and send a fail signal to 
             sender , the other one is that we send missed slice indexs to 
             sender and hope it will then sends those slices.          
          */
          uint8_t expect_status = (uint8_t)0xff >> (8 - packet->slice_size);
          //phy_layer_set_tx_addr()
          if (phy_rx_node->recv_chain != expect_status) {
            /* Some slices missed, here we dont want a ack or any compensate */
            uint8_t missed_slices = phy_rx_node->recv_chain;
            phy_rx_node->node_status = NODE_INVALID;
            /* TODO: Invalid packets are to removed from fifo */
            pr_err("expect_status=0x%02X, missed_slices=0x%02X\n", 
                                              expect_status, missed_slices);
          }
          else {
            /* Make best efforts to send success ack to sender */
            uint8_t packet_index = packet->packet_index;
            phy_rx_node->node_status = NODE_VALID;
            phy_rx_node->length = (packet->slice_size-1) * MAX_PACKET_DATA_SIZE +
                                    packet->slice_size;
            //pr_info("Rece all succ\n");
          }
          fifo_traverse(&fifo_instance);
          debug_printf("\n");
      }
      /* This is middle slices, 
         check if this node's first slice exists */
      else {
        if (packet->slice_index)
          pr_debug("Mid   %u/%u Mac:0x%04X Index:%u\n", packet->slice_index, 
            packet->slice_size - 1, *(uint16_t *)packet->src_addr, packet->packet_index);
      }

      /* Parse recv packet and fetch data into fifo node */
      if (!(phy_rx_node->recv_chain & (1 << (uint8_t)packet->slice_index))) {
        /* This slice doesnt exsits */
        uint16_t offset = packet->slice_index * MAX_PACKET_DATA_SIZE;
        memcpy(phy_rx_node->data + offset, packet->data, packet->length);
        phy_rx_node->recv_chain |= (1 << (uint8_t)packet->slice_index);
      }
    }
    else {
      pr_err("Cant handle this type message : type 0x%02X\n", packet->type);
    }
    
    exit:

    last_slice_index = packet->slice_index;
    last_src_addr = packet->src_addr;
    last_packet_index = packet->packet_index;
    return ;
  }
}

/* This function get receive data from local rx fifo.
 * @Param data: to store raw_data
 * @Param max_length: data max length to get,default to 128
 * @Return : data amount get at last 
*/
uint16_t phy_layer_get_data(uint8_t *data, uint16_t max_length = 128)
{
  
  return 0;
}

void setup()
{
  Serial.begin(500000);
  printf_begin();
 
  debug_printf("Begin config!");
  nrf_gpio_init(8, 9); //Set ce pin and csn pin
  nrf_set_tx_addr((uint8_t *)"mac00");
  nrf_set_rx_addr((uint8_t *)"mac01");
  nrf_set_broadcast_addr('a');
  
  nrf_chip_config(12, 32); // Set channel and payload
  nrf_set_retry_times(5);
  nrf_set_retry_durtion(750);
  nrf_set_channel(100);
  Serial.println("Zigbee network starts!");
  fifo_init(&fifo_instance, rx_fifo_mem, MAX_FIFO_SIZE);
  enable_rx();
}


#define RATE_SAMPLE_TIME 128
#define RATE_SAMPLE_TIME_SHIFT 7

#define MAX_RETRY_TIME 10



void loop()
{
  phy_layer_listener();

  /*
  if (phy_rx_node_instance.node_status == NODE_VALID) {
    pr_info("Detect phy_rx_node_instance valid,print data:");
    for (int i = 0; i < phy_rx_node_instance.length; i ++) {
      if (i % 15 == 0)
        debug_printf("\n");
      debug_printf("0x%02X ", phy_rx_node_instance.data[i]);
    }
    debug_printf("\n\n");
    phy_rx_node_instance.node_status = NODE_INVALID;
  }
  */
  //debug_printf("\n");
}

#ifdef test__
void loop_old()
{
  if (nrf_data_ready()) {
    uint8_t status = read_register(STATUS);
    uint8_t pipe = (status >> 1) & 0x07;
    nrf_get_data(data);
   
    send_id = data[29] < 10 ? data[29] : 0x00;
    seq_num = data[30];
    crc = data[31];
    crc_check = crc_calculate(data, 31);

    
    if (crc == crc_check) {
        debug_printf("Recv pipe:%u msg:[%s]\n", pipe, data);
      
      if ((uint8_t)seq_num == last_seq_num[send_id])
        debug_printf("[%u]Repeat seq num %u!\n", send_id, seq_num);
      else if ((uint8_t)seq_num != ((last_seq_num[send_id] + 1) % 256))
        debug_printf("[%u]Lost frame! cur=%u last=%u\n", send_id, (uint8_t)seq_num, last_seq_num[send_id]);
      last_seq_num[send_id] = seq_num;
      
      comm_sum += 32;
      
    }
    else {
      debug_printf("[%u] [%3u]Wrong crc 0x%02X != 0x%02X\n", send_id, seq_num, data, crc, crc_check);
    }
  
  }
  
}
#endif
