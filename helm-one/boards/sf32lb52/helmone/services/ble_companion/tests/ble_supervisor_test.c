/* SPDX-License-Identifier: Apache-2.0 */
/* Host regression test:
 *   make -C boards/sf32lb52/helmone/services/ble_companion/tests run
 * or:
 *   gcc -std=c11 -Wall -Wextra -Werror -pthread \
 *     -I.. -I../../../include ble_supervisor_test.c ../ble_supervisor.c \
 *     -o /tmp/ble_supervisor_test && /tmp/ble_supervisor_test
 */

#include <assert.h>

#include "ble_supervisor.h"

int main(void)
{
  enum ble_supervisor_fault reason;

  ble_supervisor_init(1000);
  assert(!ble_supervisor_cycle_requested());

  ble_supervisor_request_cycle(BLE_SUPERVISOR_FAULT_ADAPTER, 2000);
  assert(ble_supervisor_cycle_requested());
  assert(ble_supervisor_take_adapter_cycle(&reason));
  assert(reason == BLE_SUPERVISOR_FAULT_ADAPTER);

  ble_supervisor_cycle_begin(2000);
  ble_supervisor_cycle_complete(false, 3000);

  /* A failed adapter cycle must wake the pump for controller recovery. */
  assert(ble_supervisor_cycle_requested());
  assert(ble_supervisor_pending_reason() == BLE_SUPERVISOR_FAULT_ADAPTER);
  assert(!ble_supervisor_take_adapter_cycle(&reason));
  assert(ble_supervisor_take_escalation());
  assert(!ble_supervisor_cycle_requested());

  /* The five-minute controller-reset rate limit must not lose the request. */
  ble_supervisor_defer_escalation();
  assert(ble_supervisor_cycle_requested());
  assert(ble_supervisor_pending_reason() == BLE_SUPERVISOR_FAULT_ADAPTER);
  assert(!ble_supervisor_take_adapter_cycle(&reason));
  assert(ble_supervisor_take_escalation());
  ble_supervisor_escalation_done(4000);
  assert(!ble_supervisor_cycle_requested());
  return 0;
}
