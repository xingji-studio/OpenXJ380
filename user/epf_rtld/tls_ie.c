// SPDX-License-Identifier: Apache-2.0
__thread int epf_tls_ie_data = 73;

int *epf_tls_ie_address(void)
{
    return &epf_tls_ie_data;
}
