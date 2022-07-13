#include "./Console.h"
#include "soc/soc_caps.h"
#include "esp_err.h"
#include "ESP32Console/Commands/CoreCommands.h"
#include "ESP32Console/Commands/SystemCommands.h"
#include "ESP32Console/Commands/NetworkCommands.h"
#include "ESP32Console/Commands/VFSCommands.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "linenoise/linenoise.h"
#include "ESP32Console/Helpers/PWDHelpers.h"

static const char *TAG = "ESP32Console";

using namespace ESP32Console::Commands;

namespace ESP32Console
{
    void Console::registerCoreCommands()
    {
        registerCommand(getClearCommand());
        registerCommand(getHistoryCommand());
        registerCommand(getEchoCommand());
        registerCommand(getSetMultilineCommand());
        registerCommand(getEnvCommand());
        registerCommand(getDeclareCommand());
    }

    void Console::registerSystemCommands()
    {
        registerCommand(getSysInfoCommand());
        registerCommand(getRestartCommand());
        registerCommand(getMemInfoCommand());
    }

    void ESP32Console::Console::registerNetworkCommands()
    {
        registerCommand(getPingCommand());
        registerCommand(getIpconfigCommand());
    }

    void Console::registerVFSCommands()
    {
        registerCommand(getCatCommand());
        registerCommand(getCDCommand());
        registerCommand(getPWDCommand());
        registerCommand(getLsCommand());
        registerCommand(getMvCommand());
        registerCommand(getCPCommand());
        registerCommand(getRMCommand());
        registerCommand(getRMDirCommand());
        registerCommand(getEditCommand());
    }

    void Console::beginCommon()
    {
        /* Tell linenoise where to get command completions and hints */
        linenoiseSetCompletionCallback(&esp_console_get_completion);
        linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);

        /* Set command history size */
        linenoiseHistorySetMaxLen(max_history_len_);

        /* Set command maximum length */
        linenoiseSetMaxLineLen(max_cmdline_len_);

        // Load history if defined
        if (history_save_path_)
        {
            linenoiseHistoryLoad(history_save_path_);
        }

        // Register core commands like echo
        esp_console_register_help_command();
        registerCoreCommands();
    }

    void Console::begin(int baud, int rxPin, int txPin, uint8_t channel)
    {
        log_d("Initialize console");

        if (channel >= SOC_UART_NUM)
        {
            log_e("Serial number is invalid, please use numers from 0 to %u", SOC_UART_NUM - 1);
            return;
        }

        /* Drain stdout before reconfiguring it */
        fflush(stdout);
        fsync(fileno(stdout));

        /* Disable buffering on stdin */
        setvbuf(stdin, NULL, _IONBF, 0);

        /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
        esp_vfs_dev_uart_port_set_rx_line_endings(channel, ESP_LINE_ENDINGS_CR);
        /* Move the caret to the beginning of the next line on '\n' */
        esp_vfs_dev_uart_port_set_tx_line_endings(channel, ESP_LINE_ENDINGS_CRLF);

        /* Configure UART. Note that REF_TICK is used so that the baud rate remains
         * correct while APB frequency is changing in light sleep mode.
         */
        const uart_config_t uart_config = {
            .baud_rate = baud,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
#if SOC_UART_SUPPORT_REF_TICK
            .source_clk = UART_SCLK_REF_TICK,
#elif SOC_UART_SUPPORT_XTAL_CLK
            .source_clk = UART_SCLK_XTAL,
#endif
        };
        /* Install UART driver for interrupt-driven reads and writes */
        ESP_ERROR_CHECK(uart_driver_install(channel,
                                            256, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(channel, &uart_config));

        /* Tell VFS to use UART driver */
        esp_vfs_dev_uart_use_driver(channel);

        esp_console_config_t console_config = {
            .max_cmdline_length = max_cmdline_len_,
            .max_cmdline_args = max_cmdline_args_,
            .hint_color = 333333};

        ESP_ERROR_CHECK(esp_console_init(&console_config));

        /* Change standard input and output of the task if the requested UART is
         * NOT the default one. This block will replace stdin, stdout and stderr.
         */
        if (channel != CONFIG_ESP_CONSOLE_UART_NUM)
        {
            char path[12] = {0};
            snprintf(path, 12, "/dev/uart/%d", channel);

            stdin = fopen(path, "r");
            stdout = fopen(path, "w");
            stderr = stdout;
        }

        beginCommon();

        // Start REPL task
        if (xTaskCreate(&Console::repl_task, "console_repl", 4096, this, 2, &task_) != pdTRUE)
        {
            log_e("Could not start REPL task!");
        }
    }

    static void resetAfterCommands()
    {
        //Reset all global states a command could change

        //Reset getopt parameters
        optind = 0;
    }

    void Console::repl_task(void *args)
    {
        Console &console = *(static_cast<Console *>(args));

        setvbuf(stdin, NULL, _IONBF, 0);

        /* This message shall be printed here and not earlier as the stdout
         * has just been set above. */
        printf("\r\n"
               "Type 'help' to get the list of commands.\r\n"
               "Use UP/DOWN arrows to navigate through command history.\r\n"
               "Press TAB when typing command name to auto-complete.\r\n");

        // Probe terminal status
        int probe_status = linenoiseProbe();
        if (probe_status)
        {
            linenoiseSetDumbMode(1);
        }

        if (linenoiseIsDumbMode())
        {
            printf("\r\n"
                   "Your terminal application does not support escape sequences.\n\n"
                   "Line editing and history features are disabled.\n\n"
                   "On Windows, try using Putty instead.\r\n");
        }

        linenoiseSetMaxLineLen(console.max_cmdline_len_);
        while (1)
        {
            String prompt = console.prompt_;

            // Insert current PWD into prompt if needed
            prompt.replace("%pwd%", console_getpwd());

            char *line = linenoise(prompt.c_str());
            if (line == NULL)
            {
                ESP_LOGD(TAG, "empty line");
                /* Ignore empty lines */
                continue;
            }

            log_d("Line parsed: %s", line);

            /* Add the command to the history */
            linenoiseHistoryAdd(line);
            
            /* Save command history to filesystem */
            if (console.history_save_path_)
            {
                linenoiseHistorySave(console.history_save_path_);
            }

            /* Try to run the command */
            int ret;
            esp_err_t err = esp_console_run(line, &ret);
            
            //Reset global state
            resetAfterCommands();

            if (err == ESP_ERR_NOT_FOUND)
            {
                printf("Unrecognized command\n");
            }
            else if (err == ESP_ERR_INVALID_ARG)
            {
                // command was empty
            }
            else if (err == ESP_OK && ret != ESP_OK)
            {
                printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
            }
            else if (err != ESP_OK)
            {
                printf("Internal error: %s\n", esp_err_to_name(err));
            }
            /* linenoise allocates line buffer on the heap, so need to free it */
            linenoiseFree(line);
        }
        ESP_LOGD(TAG, "REPL task ended");
        vTaskDelete(NULL);
    }

    void Console::end()
    {
    }
};