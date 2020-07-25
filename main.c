/***************************************************************************************/
/*
 * beacon_scanner
 * Created by Manuel Montenegro, Sep 7, 2018.
 *
 *  This is a Bluetooth 5 scanner. This code reads every advertisement from beacons
 *  and sends its data through serial port.
 *
 *  This code has been developed for Nordic Semiconductor nRF52840 PDK.
*/
/***************************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nordic_common.h"
#include "app_uart.h"
#include "nrf_sdm.h"
#include "ble.h"
#include "ble_hci.h"
#include "ble_db_discovery.h"
#include "ble_srv_common.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "nrf_pwr_mgmt.h"
#include "app_util.h"
#include "app_error.h"
#include "ble_dis_c.h"
#include "ble_rscs_c.h"
#include "app_util.h"
#include "app_timer.h"
#include "bsp_btn_ble.h"
#include "peer_manager.h"
#include "peer_manager_handler.h"
#include "fds.h"
#include "nrf_fstorage.h"
#include "ble_conn_state.h"
#include "nrf_ble_gatt.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_ble_scan.h"

//#include "nrf_log.h"
//#include "nrf_log_ctrl.h"
//#include "nrf_log_default_backends.h"

#define UART_TX_BUF_SIZE        512                                     /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE        512                                     /**< UART RX buffer size. */


#define APP_BLE_CONN_CFG_TAG        1                                   /**< Tag that identifies the BLE configuration of the SoftDevice. */
#define APP_BLE_OBSERVER_PRIO       3                                   /**< BLE observer priority of the application. There is no need to modify this value. */
#define APP_SOC_OBSERVER_PRIO       1                                   /**< SoC observer priority of the application. There is no need to modify this value. */

#define SCAN_INTERVAL               0x0320                              /**< Determines scan interval in units of 0.625 millisecond. */
#define SCAN_WINDOW                 0x0320                              /**< Determines scan window in units of 0.625 millisecond. */
#define SCAN_DURATION               0x0000                              /**< Duration of the scanning in units of 10 milliseconds. If set to 0x0000, scanning continues until it is explicitly disabled. */

NRF_BLE_SCAN_DEF(m_scan);                                   /**< Scanning Module instance. */

static bool                  m_memory_access_in_progress;   /**< Flag to keep track of ongoing operations on persistent memory. */

static ble_gap_scan_params_t m_scan_param =                 /**< Scan parameters requested for scanning and connection. */
{
    .active        = 0x00,
    .interval      = SCAN_INTERVAL,
    .window        = SCAN_WINDOW,
    .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    .timeout       = SCAN_DURATION,
//    .scan_phys     = BLE_GAP_PHY_CODED,                                 // Choose only one of the following scan_phys
    .scan_phys     = BLE_GAP_PHY_1MBPS,
//    .scan_phys     = BLE_GAP_PHY_2MBPS,
    .extended      = 1,
};

static void scan_start(void);


void uart_print(char* buffer)
{
    int size = strlen(buffer);
    int i=0;
    while(size-- > 0)
    {
        app_uart_put(buffer[i++]);
    }
}

void uart_hex_dump(uint8_t* p_data, int length, bool with_colons)
{
    uint8_t value;
    int index = 0;
    while(length-- > 0)
    {
        value = p_data[index]>>4;
        if(value <= 9)
        {
            app_uart_put('0'+value);
        }
        else
        {
            app_uart_put('A'+value-10);
        }

        value = p_data[index]&0x0F;
        if(value <= 9)
        {
            app_uart_put('0'+value);
        }
        else
        {
            app_uart_put('A'+value-10);
        }

        if(length > 0)
        {
            if(with_colons)
            {
                app_uart_put(':');
            }
        }
        else
        {
            app_uart_put('\r');
            app_uart_put('\n');
        }

        index++;
    }
}


static void send_sigfox(uint8_t* p_data)
{
    uart_print("AT&RC\r\n");
    uart_print("AT&SF=");
    uart_hex_dump(&m_scan.scan_buffer.p_data[25], 6, false);
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    static int beacon_count = 0;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_REPORT:
        {
//            NRF_LOG_RAW_HEXDUMP_INFO (m_scan.scan_buffer.p_data, m_scan.scan_buffer.len);
//            NRF_LOG_RAW_INFO ("----------------------------------\r\n");

            if(m_scan.scan_buffer.p_data[3] == 0x1A &&
			   m_scan.scan_buffer.p_data[4] == 0xFF &&
               m_scan.scan_buffer.p_data[7] == 0x02 &&
               m_scan.scan_buffer.p_data[8] == 0x15)
            {
                ble_gap_evt_adv_report_t const * p_adv_report = &p_ble_evt->evt.gap_evt.params.adv_report;
                int8_t rssi = -1;
                if(p_adv_report)
                {
                    rssi = p_adv_report->rssi;
                }

                m_scan.scan_buffer.p_data[30] = rssi;

                uart_print("B: ");
				uart_hex_dump(m_scan.scan_buffer.p_data, 31, true);
                uart_print("=======\r\n\n");

                if(0 == (beacon_count & 0x000000ff))
				{
                    send_sigfox(m_scan.scan_buffer.p_data);
				}

                beacon_count++;
            }
        }

        default:
            break;
    }
}


/**
 * @brief SoftDevice SoC event handler.
 *
 * @param[in] evt_id    SoC event.
 * @param[in] p_context Context.
 */
static void soc_evt_handler(uint32_t evt_id, void * p_context)
{
    switch (evt_id)
    {
        case NRF_EVT_FLASH_OPERATION_SUCCESS:
        /* fall through */
        case NRF_EVT_FLASH_OPERATION_ERROR:

            if (m_memory_access_in_progress)
            {
                m_memory_access_in_progress = false;
                scan_start();
            }
            break;

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
  */
static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    // Register handlers for BLE and SoC events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
    NRF_SDH_SOC_OBSERVER(m_soc_observer, APP_SOC_OBSERVER_PRIO, soc_evt_handler, NULL);
}


/**@brief Function for handling Scanning Module events.
 */
static void scan_evt_handler(scan_evt_t const * p_scan_evt)
{
    switch(p_scan_evt->scan_evt_id)
    {
        case NRF_BLE_SCAN_EVT_SCAN_TIMEOUT:
        {
//            NRF_LOG_INFO("Scan timed out.");
            scan_start();
        } break;

        default:
          break;
    }
}


/**@brief Function for initializing the scanning and setting the filters.
 */
static void scan_init(void)
{
    ret_code_t          err_code;
    nrf_ble_scan_init_t init_scan;

    memset(&init_scan, 0, sizeof(init_scan));

    init_scan.connect_if_match = false;
    init_scan.conn_cfg_tag     = APP_BLE_CONN_CFG_TAG;
    init_scan.p_scan_param     = &m_scan_param;

    err_code = nrf_ble_scan_init(&m_scan, &init_scan, scan_evt_handler);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for starting scanning.
 */
static void scan_start(void)
{
    ret_code_t err_code;

    // If there is any pending write to flash, defer scanning until it completes.
    if (nrf_fstorage_is_busy(NULL))
    {
        m_memory_access_in_progress = true;
        return;
    }

    err_code = nrf_ble_scan_start(&m_scan);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing logging. */
//static void log_init(void)
//{
//    ret_code_t err_code = NRF_LOG_INIT(NULL);
//    APP_ERROR_CHECK(err_code);

//    NRF_LOG_DEFAULT_BACKENDS_INIT();
//}


/**@brief Function for initializing the timer. */
static void timer_init(void)
{
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing power management.
 */
static void power_management_init(void)
{
    ret_code_t err_code;
    err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the idle state (main loop).
 *
 * @details Handles any pending log operations, then sleeps until the next event occurs.
 */
static void idle_state_handle(void)
{
//    if (NRF_LOG_PROCESS() == false)
    {
        nrf_pwr_mgmt_run();
    }
}


/**@brief   Function for handling app_uart events.
 *
 * @details This function receives a single character from the app_uart module and appends it to
 *          a string. The string is sent over BLE when the last character received is a
 *          'new line' '\n' (hex 0x0A) or if the string reaches the maximum data length.
 */
void uart_event_handle(app_uart_evt_t * p_event)
{
    static uint8_t data_array[UART_RX_BUF_SIZE];
    static uint16_t index = 0;

    switch (p_event->evt_type)
    {
        /**@snippet [Handling data from UART] */
        case APP_UART_DATA_READY:
            UNUSED_VARIABLE(app_uart_get(&data_array[index]));

            app_uart_put(data_array[index]);

            index++;

            if ((data_array[index - 1] == '\n') ||
                (data_array[index - 1] == '\r') ||
                (index >= UART_RX_BUF_SIZE))
            {
//                NRF_LOG_DEBUG("UART Received");
//                NRF_LOG_HEXDUMP_DEBUG(data_array, index);

                // TODO - check for "OK" response from AT Command

                index = 0;
            }
            break;

        /**@snippet [Handling data from UART] */
        case APP_UART_COMMUNICATION_ERROR:
//            NRF_LOG_ERROR("Communication error occurred while handling UART.");
            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
//            NRF_LOG_ERROR("Error occurred in FIFO module used by UART.");
            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        default:
            break;
    }
}


/**@brief Function for initializing the UART. */
static void uart_init(void)
{
    // TODO - try to create a second uart
    ret_code_t err_code;

    app_uart_comm_params_t const comm_params =
    {
        .rx_pin_no    = RX_PIN_NUMBER,
        .tx_pin_no    = TX_PIN_NUMBER,
        .rts_pin_no   = RTS_PIN_NUMBER,
        .cts_pin_no   = CTS_PIN_NUMBER,
        .flow_control = APP_UART_FLOW_CONTROL_DISABLED,
        .use_parity   = false,
        .baud_rate    = UART_BAUDRATE_BAUDRATE_Baud9600
    };

    APP_UART_FIFO_INIT(&comm_params,
                       UART_RX_BUF_SIZE,
                       UART_TX_BUF_SIZE,
                       uart_event_handle,
                       APP_IRQ_PRIORITY_LOWEST,
                       err_code);

    APP_ERROR_CHECK(err_code);
}


int main(void)
{
    // Initialize.
//    log_init();
    uart_init();
    timer_init();
    power_management_init();
    ble_stack_init();
    scan_init();

    // Start execution.
    uart_print("\r\n\r\n");
    uart_print("------------------\r\n");
    uart_print("| BEACON SCANNER |\r\n");
    uart_print("------------------\r\n");

    scan_start();


    // Enter main loop.
    for (;;)
    {
        idle_state_handle();
    }
}
