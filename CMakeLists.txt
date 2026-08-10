cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# NOTE: do not try to fix pppos_input_tcpip ERR_MEM by raising
# MEMP_NUM_TCPIP_MSG_INPKT or PBUF_POOL_SIZE here. IDF sets MEMP_MEM_MALLOC=1,
# and in that mode lwIP's do_memp_malloc_pool() goes straight to mem_malloc()
# and only enforces a count limit for MEMP_TCP_PCB — every other MEMP_NUM_* is
# ignored. Measured on hardware: heap_min stayed at ~187 KB while the errors
# flooded, confirming allocation was never failing. The mailbox is the limit.

project(TTGO-T-Internet-COM-Router)
