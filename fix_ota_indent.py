#!/usr/bin/env python3
"""Fix indentation in ota_check_latest_version function"""
with open('main/app/app_ota.c', 'r') as f:
    content = f.read()

# The problematic section is:
#     } else {
#         ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
#         s_ota_state = OTA_STATE_ERROR;
#     }
#     } else {
#         ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
#         s_ota_state = OTA_STATE_ERROR;
#     }
#
# It should be:
#     } else {
#         ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
#         s_ota_state = OTA_STATE_ERROR;
#     }
# } else {
#     ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
#     s_ota_state = OTA_STATE_ERROR;
# }

old = '''    } else {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        s_ota_state = OTA_STATE_ERROR;
    }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
    }'''

new = '''    } else {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        s_ota_state = OTA_STATE_ERROR;
    }
} else {
    ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    s_ota_state = OTA_STATE_ERROR;
}'''

if old in content:
    content = content.replace(old, new, 1)
    with open('main/app/app_ota.c', 'w') as f:
        f.write(content)
    print("Fixed indentation successfully")
else:
    print("Pattern not found, showing current state:")
    lines = content.split('\n')
    for i in range(129, min(145, len(lines))):
        print(f'{i+1}: {lines[i]}')