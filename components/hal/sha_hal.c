// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The HAL layer for SHA

#include "hal/sha_hal.h"
#include "hal/sha_types.h"
#include "hal/sha_ll.h"
#include "soc/soc_caps.h"
#include <stdlib.h>
#include <stdio.h>

#if SOC_SHA_CRYPTO_DMA
#include "soc/crypto_dma_reg.h"
#include "hal/crypto_dma_ll.h"
#elif SOC_SHA_GENERAL_DMA
#include "hal/gdma_ll.h"
#endif

#define SHA1_STATE_LEN_WORDS    (160 / 32)
#define SHA256_STATE_LEN_WORDS  (256 / 32)
#define SHA512_STATE_LEN_WORDS  (512 / 32)

#if CONFIG_IDF_TARGET_ESP32

/* Return state size (in words) for a given SHA type */
inline static size_t state_length(esp_sha_type type)
{
    switch (type) {
    case SHA1:
        return SHA1_STATE_LEN_WORDS;
    case SHA2_256:
        return SHA256_STATE_LEN_WORDS;
    case SHA2_384:
    case SHA2_512:
        return SHA512_STATE_LEN_WORDS;
    default:
        return 0;
    }
}

#else
/* Return state size (in words) for a given SHA type */
inline static size_t state_length(esp_sha_type type)
{
    switch (type) {
    case SHA1:
        return SHA1_STATE_LEN_WORDS;
    case SHA2_224:
    case SHA2_256:
        return SHA256_STATE_LEN_WORDS;
#if SOC_SHA_SUPPORT_SHA384
    case SHA2_384:
        return SHA512_STATE_LEN_WORDS;
#endif
#if SOC_SHA_SUPPORT_SHA512
    case SHA2_512:
        return SHA512_STATE_LEN_WORDS;
#endif
#if SOC_SHA_SUPPORT_SHA512_T
    case SHA2_512224:
    case SHA2_512256:
    case SHA2_512T:
        return SHA512_STATE_LEN_WORDS;
#endif
    default:
        return 0;
    }
}
#endif


/* Hash a single block */
void sha_hal_hash_block(esp_sha_type sha_type, const void *data_block, size_t block_word_len, bool first_block)
{
    sha_hal_wait_idle();

    sha_ll_fill_text_block(data_block, block_word_len);

    /* Start hashing */
    if (first_block) {
        sha_ll_start_block(sha_type);
    } else {
        sha_ll_continue_block(sha_type);
    }
}

#if SOC_SHA_SUPPORT_DMA

#if SOC_SHA_GENERAL_DMA
static inline void sha_hal_dma_init(lldesc_t *input)
{
    /* Update driver when centralized DMA interface implemented, IDF-2192 */
    gdma_ll_tx_enable_descriptor_burst(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, false);
    gdma_ll_tx_enable_data_burst(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, false);
    gdma_ll_tx_enable_auto_write_back(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, false);

    gdma_ll_tx_connect_to_periph(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, GDMA_LL_PERIPH_ID_SHA);

#if SOC_GDMA_SUPPORT_EXTMEM
    /* Atleast 40 bytes when accessing external RAM */
    gdma_ll_tx_extend_fifo_size_to(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, 40);
    gdma_ll_tx_set_block_size_psram(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, GDMA_OUT_EXT_MEM_BK_SIZE_16B);
#endif //SOC_GDMA_SUPPORT_EXTMEM

    /* Set descriptors */
    gdma_ll_tx_set_desc_addr(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL, (uint32_t)input);

    gdma_ll_rx_reset_channel(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL);
    gdma_ll_tx_reset_channel(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL);

    /* Start transfer */
    gdma_ll_tx_start(&GDMA, SOC_GDMA_SHA_DMA_CHANNEL);
}
#endif //SOC_SHA_GENERAL_DMA



#if SOC_SHA_CRYPTO_DMA
static inline void sha_hal_dma_init(lldesc_t *input)
{
    crypto_dma_ll_set_mode(CRYPTO_DMA_SHA);
    crypto_dma_ll_reset();

    crypto_dma_ll_outlink_set((uint32_t)input);
    crypto_dma_ll_outlink_start();
}
#endif

/* Hashes a number of message blocks using DMA */
void sha_hal_hash_dma(esp_sha_type sha_type, lldesc_t *input, size_t num_blocks, bool first_block)
{
    sha_hal_wait_idle();

    sha_hal_dma_init(input);

    sha_ll_set_block_num(num_blocks);

    /* Start hashing */
    if (first_block) {
        sha_ll_start_dma(sha_type);
    } else {
        sha_ll_continue_dma(sha_type);
    }
}

#endif //SOC_SHA_SUPPORT_DMA

void sha_hal_wait_idle()
{
    while (sha_ll_busy()) {
    }
}

/* Reads the current message digest from the SHA engine */
void sha_hal_read_digest(esp_sha_type sha_type, void *digest_state)
{
    uint32_t *digest_state_words = (uint32_t *)digest_state;

    sha_ll_load(sha_type);
    uint32_t word_len = state_length(sha_type);

    sha_hal_wait_idle();
    sha_ll_read_digest(sha_type, digest_state, word_len);

    /* Fault injection check: verify SHA engine actually ran,
       state is not all zeroes.
    */
    for (int i = 0; i < word_len; i++) {
        if (digest_state_words[i] != 0) {
            return;
        }
    }
    abort(); // SHA peripheral returned all zero state, probably due to fault injection
}

#if SOC_SHA_SUPPORT_RESUME
/* Writes the message digest to the SHA engine */
void sha_hal_write_digest(esp_sha_type sha_type, void *digest_state)
{
    sha_ll_write_digest(sha_type, digest_state, state_length(sha_type));
}
#endif //SOC_SHA_SUPPORT_RESUME

#if SOC_SHA_SUPPORT_SHA512_T

/* Calculates and sets the initial digiest for SHA512_t */
void sha_hal_sha512_init_hash(uint32_t t_string, uint8_t t_len)
{
    sha_ll_t_string_set(t_string);
    sha_ll_t_len_set(t_len);

    sha_ll_start_block(SHA2_512T);

    sha_hal_wait_idle();
}
#endif //SOC_SHA_SUPPORT_SHA512_T
