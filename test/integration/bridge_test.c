
#include <assert.h>
#include <stdio.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#include "components/bridge/qn_bridge.c"
TickType_t mock_tick;
static unsigned mono_calls,deemph_calls,complete_calls;
static unsigned reconciles;
static uint32_t completed_generation;
static esp_err_t tune_error;
esp_err_t radio_add_listener(radio_listener_t cb,void *ctx){return ESP_OK;}
esp_err_t radio_set_mute(bool m){return ESP_OK;}
esp_err_t radio_set_force_mono(bool m){mono_calls++;return ESP_OK;}
esp_err_t radio_set_deemphasis(uint16_t d){deemph_calls++;return ESP_OK;}
esp_err_t radio_set_volume(int16_t v){return ESP_OK;}
esp_err_t radio_tune_resume(uint32_t k){return tune_error;}
esp_err_t radio_tune_keep_programme(uint32_t k){return tune_error;}
esp_err_t radio_scan(uint32_t a,uint32_t z,uint32_t s){return ESP_OK;}
esp_err_t qn8035_slave_start(const qn8035_slave_config_t *c,qn8035_slave_event_cb_t cb,void *ctx){return ESP_OK;}
void qn8035_slave_complete_tune(uint32_t k,bool f,bool s,uint32_t g){complete_calls++;completed_generation=g;}
void qn8035_slave_request_reconcile(void){reconciles++;}
void qn8035_slave_set_rds_sync(bool sync){}
void qn8035_slave_set_signal(uint8_t r,uint8_t n,bool s){}
void qn8035_slave_push_rds(const uint16_t b[4],uint8_t e,bool v,bool s){}
int main(void){
    assert(qn_bridge_init()==ESP_OK);
    qn8035_emu_t e={.receiving=true,.muted=false};
    e.regs[QN8035_REG_SYSTEM1]=QN8035_SYSTEM1_RXREQ|QN8035_SYSTEM1_FORCE_MO;
    e.regs[QN8035_REG_VOL_CTL]=QN8035_VOL_TC;
    qn8035_slave_config_t c={.restore=&e};
    assert(qn_bridge_start(&c)==ESP_OK);
    assert(mono_calls==1 && deemph_calls==1);
    puts("PASS warm restore reapplies mono and de-emphasis");
    begin_op(OP_TUNE,95000,7);mock_tick=10000;
    assert(b.op==OP_TUNE && !complete_calls);
    deadline_timer_cb(NULL);
    assert(b.op==OP_NONE && complete_calls==1 && completed_generation==7);
    puts("PASS independent deadline completes tune without quality callbacks");
    begin_op(OP_TUNE,96000,8);fail_op(95000,false,7);
    assert(b.op==OP_TUNE && b.generation==8);
    puts("PASS old-generation completion cannot clear a newer bridge operation");
    b.op=OP_NONE;tune_error=ESP_ERR_TIMEOUT;
    start_tune(97000,9);assert(reconciles>0 && !b.posted);
    const TickType_t first=b.t;mock_tick+=100;start_tune(97000,9);
    assert(b.t==first);
    puts("PASS queue retry requests reconciliation without extending tune deadline");
    return 0;
}
