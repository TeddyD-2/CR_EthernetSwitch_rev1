/**
  ******************************************************************************
  * @file    ethernetif_h7.cpp
  * @brief   STM32H7 + KSZ9477 MAC-to-MAC lwIP network interface.
  *
  * Replaces the legacy F4/F7 ethernetif.cpp on STM32H7 targets. Uses the
  * modern HAL ETH API (enhanced DMA descriptors, HAL_ETH_Transmit /
  * HAL_ETH_ReadData) and assumes a KSZ9477 switch is driving the RMII link
  * at forced 100 Mbps / full duplex — no MDIO / PHY access from the STM32.
  *
  * Hardware expectations
  * ---------------------
  *   - 50 MHz RMII REFCLK is produced BY the KSZ9477 and consumed by the
  *     STM32 on PA1 (ETH_REF_CLK). KSZ strapping: RXD3=0, RXD2=1, RXD1=0,
  *     RXD0=1 (RMII clock-mode + LED strap, see KSZ9477S datasheet).
  *   - KSZ9477 port 6 is programmed over SPI to RMII + 100M + FD + TX/RX on
  *     BEFORE this driver starts. See README_H7.md.
  *   - No PHY on the STM32 side of the link.
  *
  * Memory layout
  * -------------
  * DMA descriptors and buffers live in D2 SRAM (0x30040000) via linker
  * sections .RxDecripSection / .TxDecripSection / .RxArraySection /
  * .TxArraySection. An MPU region marks the block as Normal, non-cacheable,
  * non-bufferable, shareable. See README_H7.md for the linker fragment.
  ******************************************************************************
  */

#include "stm32_def.h"

#if defined(STM32H7xx)

#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "lwip/igmp.h"
#include "netif/etharp.h"
#include <string.h>

#include "ethernetif.h"
#include "stm32_eth.h"
#include "PeripheralPins.h"
#if !defined(STM32_CORE_VERSION) || (STM32_CORE_VERSION <= 0x01050000)
  #include "variant.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* H7 enhanced descriptors are 32 bytes. 4 x 1536-byte buffers per direction
 * = 12 KB total, fits comfortably in D2 SRAM3 (32 KB). */
#define ETH_RX_DESC_CNT     4
#define ETH_TX_DESC_CNT     4
#define ETH_RX_BUFFER_SIZE  1536
#define ETH_TX_BUFFER_SIZE  1536

/* Default MAC fallback (identical policy to legacy ethernetif.cpp). */
#if !defined(MAC_ADDR0)
#define MAC_ADDR0   0x00
#endif
#if !defined(MAC_ADDR1)
#define MAC_ADDR1   0x80
#endif
#if !defined(MAC_ADDR2)
#define MAC_ADDR2   0xE1
#endif
#if !defined(MAC_ADDR3)
#define MAC_ADDR3   ((uint8_t)(((*(uint32_t *)UID_BASE) & 0x00FF0000) >> 16))
#endif
#if !defined(MAC_ADDR4)
#define MAC_ADDR4   ((uint8_t)(((*(uint32_t *)UID_BASE) & 0x0000FF00) >> 8))
#endif
#if !defined(MAC_ADDR5)
#define MAC_ADDR5   ((uint8_t)((*(uint32_t *)UID_BASE) & 0x000000FF))
#endif
static uint8_t macaddress[6] = { MAC_ADDR0, MAC_ADDR1, MAC_ADDR2,
                                 MAC_ADDR3, MAC_ADDR4, MAC_ADDR5 };

/* Forced link parameters. The KSZ9477 is programmed to match these over SPI
 * on its switch side; this only tells the STM32 MAC what to expect. */
#ifndef ETHERNETIF_H7_FORCED_SPEED
#define ETHERNETIF_H7_FORCED_SPEED      ETH_SPEED_100M
#endif
#ifndef ETHERNETIF_H7_FORCED_DUPLEX
#define ETHERNETIF_H7_FORCED_DUPLEX     ETH_FULLDUPLEX_MODE
#endif

/* ---------- DMA descriptors & buffers, placed in D2 SRAM ----------------- */
/* ST-canonical (misspelled) section names are used unchanged so existing
 * linker fragments from ST H7 examples drop in without edits. */
__attribute__((section(".RxDecripSection"), aligned(32)))
static ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];

__attribute__((section(".TxDecripSection"), aligned(32)))
static ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];

__attribute__((section(".RxArraySection"), aligned(32)))
static uint8_t Rx_Buff[ETH_RX_DESC_CNT][ETH_RX_BUFFER_SIZE];

__attribute__((section(".TxArraySection"), aligned(32)))
static uint8_t Tx_Buff[ETH_TX_DESC_CNT][ETH_TX_BUFFER_SIZE];

static ETH_HandleTypeDef EthHandle;
static ETH_TxPacketConfigTypeDef TxConfig;
static uint32_t txBufIdx = 0;

/* Bitmask of free Rx_Buff[] slots. HAL_ETH_Start asks for ETH_RX_DESC_CNT
 * buffers up-front via HAL_ETH_RxAllocateCallback, and each ReadData cycle
 * hands a buffer back through HAL_ETH_RxLinkCallback. */
static volatile uint32_t rxFreeMask = (1UL << ETH_RX_DESC_CNT) - 1UL;

#if LWIP_IGMP
uint32_t ETH_HashTableHigh = 0x0;
uint32_t ETH_HashTableLow  = 0x0;
#endif

/* ---------- MPU configuration -------------------------------------------- */
/* Mark the first 64 KB of D2 SRAM (0x30040000..0x3004FFFF) as Normal, not
 * cacheable, not bufferable, shareable — mandatory for coherent ETH DMA.
 * __weak so a user can override if D2 SRAM is shared with other DMAs. */
__weak void ethernetif_h7_mpu_config(void)
{
  MPU_Region_InitTypeDef r = {0};

  HAL_MPU_Disable();

  r.Enable           = MPU_REGION_ENABLE;
  r.Number           = MPU_REGION_NUMBER0;
  r.BaseAddress      = 0x30040000UL;
  r.Size             = MPU_REGION_SIZE_64KB;
  r.AccessPermission = MPU_REGION_FULL_ACCESS;
  r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  r.IsShareable      = MPU_ACCESS_SHAREABLE;
  r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  r.TypeExtField     = MPU_TEX_LEVEL0;
  r.SubRegionDisable = 0x00;
  HAL_MPU_ConfigRegion(&r);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/* ---------- HAL ETH MSP --------------------------------------------------- */
void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
  GPIO_InitTypeDef GPIO_InitStructure;
  const PinMap *map = PinMap_Ethernet;
  PinName pin = pin_pinName(map);
  GPIO_TypeDef *port;

  UNUSED(heth);

  /* SYSCFG clock must be on and PMCR programmed to RMII BEFORE ETH clocks
   * come up on H7. */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII);

  /* D2 SRAM3 hosts DMA descriptors and buffers. Without this clock enable,
   * descriptors read back as zero and TX silently drops. */
  __HAL_RCC_D2SRAM3_CLK_ENABLE();

  __HAL_RCC_ETH1MAC_CLK_ENABLE();
  __HAL_RCC_ETH1TX_CLK_ENABLE();
  __HAL_RCC_ETH1RX_CLK_ENABLE();

  /* Configure all ETH pins from the variant's PinMap_Ethernet table. */
  if (map != NULL) {
    while (pin != NC) {
      port = set_GPIO_Port_Clock(STM_PORT(pin));
      GPIO_InitStructure.Pin       = STM_GPIO_PIN(pin);
      GPIO_InitStructure.Mode      = STM_PIN_MODE(pinmap_function(pin, PinMap_Ethernet));
      GPIO_InitStructure.Pull      = STM_PIN_PUPD(pinmap_function(pin, PinMap_Ethernet));
      GPIO_InitStructure.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
      GPIO_InitStructure.Alternate = STM_PIN_AFNUM(pinmap_function(pin, PinMap_Ethernet));
      HAL_GPIO_Init(port, &GPIO_InitStructure);
      pin = pin_pinName(++map);
    }
  }

#ifdef ETH_INPUT_USE_IT
  HAL_NVIC_SetPriority(ETH_IRQn, 0x7, 0);
  HAL_NVIC_EnableIRQ(ETH_IRQn);
#endif
}

/* ---------- LL driver: init, tx, rx -------------------------------------- */
static void low_level_init(struct netif *netif)
{
  /* Configure MPU for the DMA region BEFORE any DMA can run. */
  ethernetif_h7_mpu_config();

  EthHandle.Instance            = ETH;
  EthHandle.Init.MACAddr        = macaddress;
  EthHandle.Init.MediaInterface = HAL_ETH_RMII_MODE;
  EthHandle.Init.RxDesc         = DMARxDscrTab;
  EthHandle.Init.TxDesc         = DMATxDscrTab;
  EthHandle.Init.RxBuffLen      = ETH_RX_BUFFER_SIZE;

  if (HAL_ETH_Init(&EthHandle) != HAL_OK) {
    return; /* Hardware init failed — leave netif link down. */
  }

  /* No explicit RX buffer assignment here: HAL_ETH_Start() calls
   * HAL_ETH_RxAllocateCallback() once per descriptor to populate DESC0. */

  /* TX packet template reused for every transmit. */
  memset(&TxConfig, 0, sizeof(TxConfig));
  TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM |
                          ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl   = ETH_CRC_PAD_INSERT;

  /* MAC-to-MAC: no autoneg, force speed/duplex. MUST happen between
   * HAL_ETH_Init and HAL_ETH_Start — setting MACCR after start silently
   * fails on H7. */
  ETH_MACConfigTypeDef macConfig;
  HAL_ETH_GetMACConfig(&EthHandle, &macConfig);
  macConfig.DuplexMode      = ETHERNETIF_H7_FORCED_DUPLEX;
  macConfig.Speed           = ETHERNETIF_H7_FORCED_SPEED;
  macConfig.ChecksumOffload = ENABLE;
  HAL_ETH_SetMACConfig(&EthHandle, &macConfig);

  /* lwIP netif setup */
  netif->hwaddr_len = ETH_HWADDR_LEN;
  memcpy(netif->hwaddr, macaddress, 6);
  netif->mtu   = 1500;
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  /* KSZ9477 is the link master — declare link up unconditionally. */
  netif->flags |= NETIF_FLAG_LINK_UP;

#ifdef ETH_INPUT_USE_IT
  HAL_ETH_Start_IT(&EthHandle);
#else
  HAL_ETH_Start(&EthHandle);
#endif

#if LWIP_IGMP
  netif_set_igmp_mac_filter(netif, igmp_mac_filter);
#endif
}

/* Transmit one pbuf chain. pbufs live in cacheable SRAM, so flatten into a
 * non-cacheable D2 TX buffer and hand that to the DMA — avoids the
 * SCB_CleanDCache dance entirely. */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  UNUSED(netif);

  if (p == NULL || p->tot_len == 0 || p->tot_len > ETH_TX_BUFFER_SIZE) {
    return ERR_IF;
  }

  uint8_t *txBuf = Tx_Buff[txBufIdx];
  txBufIdx = (txBufIdx + 1) % ETH_TX_DESC_CNT;

  uint16_t copied = pbuf_copy_partial(p, txBuf, p->tot_len, 0);
  if (copied != p->tot_len) {
    return ERR_BUF;
  }

  ETH_BufferTypeDef txBuffer;
  memset(&txBuffer, 0, sizeof(txBuffer));
  txBuffer.buffer = txBuf;
  txBuffer.len    = copied;
  txBuffer.next   = NULL;

  TxConfig.Length   = copied;
  TxConfig.TxBuffer = &txBuffer;
  TxConfig.pData    = NULL;

  if (HAL_ETH_Transmit(&EthHandle, &TxConfig, 100) != HAL_OK) {
    return ERR_IF;
  }
  return ERR_OK;
}

/* Pull one received frame from the DMA. Returns NULL when idle.
 *
 * HAL_ETH_ReadData() walks owned RX descriptors, calls HAL_ETH_RxLinkCallback()
 * once per descriptor (below — copies into a fresh pbuf chain), then refills
 * descriptors by calling HAL_ETH_RxAllocateCallback() (also below). */
static struct pbuf *low_level_input(struct netif *netif)
{
  UNUSED(netif);
  struct pbuf *p = NULL;
  HAL_ETH_ReadData(&EthHandle, (void **)&p);
  return p;
}

/* ---------- HAL weak callbacks -------------------------------------------
 * USE_HAL_ETH_REGISTER_CALLBACKS is 0 in the Arduino core's hal_conf, so the
 * HAL calls these by name (both are __weak in the HAL source). Override
 * here to wire the ETH DMA into lwIP's pbuf pool with a tiny free-mask
 * over the Rx_Buff[] slots in D2 SRAM. */

extern "C" void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
    if (rxFreeMask & (1UL << i)) {
      rxFreeMask &= ~(1UL << i);
      *buff = Rx_Buff[i];
      return;
    }
  }
  *buff = NULL;     /* all buffers held by pending frames — HAL will retry */
}

extern "C" void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd,
                                       uint8_t *buff, uint16_t Length)
{
  struct pbuf *p = pbuf_alloc(PBUF_RAW, Length, PBUF_POOL);
  if (p != NULL) {
    if (pbuf_take(p, buff, Length) != ERR_OK) {
      pbuf_free(p);
      p = NULL;
    }
  }

  if (p != NULL) {
    if (*pStart == NULL) {
      *pStart = p;
      *pEnd   = p;
    } else {
      pbuf_cat((struct pbuf *)*pEnd, p);
      *pEnd = p;
    }
  }

  /* Data has been copied into pbuf (or dropped) — the D2 SRAM buffer is
   * free again. Compute its index by pointer arithmetic against Rx_Buff[]. */
  uintptr_t offset = (uintptr_t)buff - (uintptr_t)&Rx_Buff[0][0];
  uint32_t idx = (uint32_t)(offset / ETH_RX_BUFFER_SIZE);
  if (idx < ETH_RX_DESC_CNT) {
    rxFreeMask |= (1UL << idx);
  }
}

/* ---------- lwIP-facing glue --------------------------------------------- */

void ethernetif_input(struct netif *netif)
{
  struct pbuf *p = low_level_input(netif);
  if (p == NULL) {
    return;
  }
  if (netif->input(p, netif) != ERR_OK) {
    pbuf_free(p);
  }
}

uint8_t ethernetif_is_init(void)
{
  return (EthHandle.gState != HAL_ETH_STATE_RESET);
}

err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "lwip";
#endif

  netif->name[0]    = IFNAME0;
  netif->name[1]    = IFNAME1;
  netif->output     = etharp_output;
  netif->linkoutput = low_level_output;

  low_level_init(netif);
  return ERR_OK;
}

u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* ---------- Link state: KSZ is always up from STM32's perspective -------- */

void ethernetif_set_link(struct netif *netif)
{
  /* No PHY / MDIO. KSZ9477 is programmed over SPI before this driver starts
   * and does not change link state on its own, so the netif stays up. */
  if (!netif_is_link_up(netif)) {
    netif_set_link_up(netif);
  }
}

void ethernetif_update_config(struct netif *netif)
{
  if (netif_is_link_up(netif)) {
    HAL_ETH_Start(&EthHandle);
  } else {
    HAL_ETH_Stop(&EthHandle);
  }
  ethernetif_notify_conn_changed(netif);
}

__weak void ethernetif_notify_conn_changed(struct netif *netif)
{
  UNUSED(netif);
}

/* ---------- MAC address accessors (consumed by stm32_eth.cpp) ------------ */

void ethernetif_set_mac_addr(const uint8_t *mac)
{
  if (mac != NULL && !ethernetif_is_init()) {
    memcpy(macaddress, mac, 6);
  }
}

void ethernetif_get_mac_addr(uint8_t *mac)
{
  if (mac != NULL) {
    memcpy(mac, macaddress, 6);
  }
}

/* ---------- IGMP multicast hash ------------------------------------------ */

#if LWIP_IGMP

#ifndef HASH_BITS
#define HASH_BITS 6
#endif

static uint32_t ethcrc(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < length; i++) {
    for (int j = 0; j < 8; j++) {
      if (((crc >> 31) ^ (data[i] >> j)) & 0x01) {
        crc = (crc << 1) ^ 0x04C11DB7;
      } else {
        crc = crc << 1;
      }
    }
  }
  return ~crc;
}

void register_multicast_address(const uint8_t *mac)
{
  uint32_t crc = ethcrc(mac, HASH_BITS);
  uint8_t hash = (crc >> 26) & 0x3F;

  /* H7 register names: MACHT1R = high 32 bits, MACHT0R = low 32 bits
   * (F4/F7 legacy used MACHTHR/MACHTLR). */
  if (hash > 31) {
    ETH_HashTableHigh |= 1UL << (hash - 32);
    EthHandle.Instance->MACHT1R = ETH_HashTableHigh;
  } else {
    ETH_HashTableLow  |= 1UL << hash;
    EthHandle.Instance->MACHT0R = ETH_HashTableLow;
  }
}

err_t igmp_mac_filter(struct netif *netif, const ip4_addr_t *ip4_addr, netif_mac_filter_action action)
{
  uint8_t mac[6];
  const uint8_t *p = (const uint8_t *)ip4_addr;
  UNUSED(netif);
  UNUSED(action);

  mac[0] = 0x01;
  mac[1] = 0x00;
  mac[2] = 0x5E;
  mac[3] = *(p + 1) & 0x7F;
  mac[4] = *(p + 2);
  mac[5] = *(p + 3);

  register_multicast_address(mac);
  return 0;
}
#endif /* LWIP_IGMP */

/* ---------- Interrupt-mode hook (optional) ------------------------------- */
#ifdef ETH_INPUT_USE_IT
extern struct netif gnetif;

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
  UNUSED(heth);
  ethernetif_input(&gnetif);
}

void ETH_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&EthHandle);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* defined(STM32H7xx) */
/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
