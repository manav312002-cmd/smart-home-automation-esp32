/*
   Template for Device-Specific RainMaker Implementation
   
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once
#include <esp_rmaker_core.h>

/* Required functions that app_main.c calls */
void app_driver_init(void);
esp_rmaker_node_t *app_device_create_node(esp_rmaker_config_t *config);
esp_rmaker_device_t *app_device_create(esp_rmaker_node_t *node);
esp_err_t app_device_set_mfg_data(void);
void app_led_set_connected(void);