
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#define setenv mock_setenv
#define settimeofday mock_settimeofday
#define localtime_r mock_localtime_r
static int mock_setenv(const char *k,const char *v,int overwrite) { return 0; }
static int mock_settimeofday(const struct timeval *tv,const void *tz) { return 0; }
static struct tm *mock_localtime_r(const long *t,struct tm *v) { time_t host_time=*t;*v=*localtime(&host_time);return v; }
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s); if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l; }
#include "components/radio/radio.c"
TickType_t mock_tick;
static uint32_t hw_khz;
static tef6686_tune_mode_t hw_mode;
static bool fail_jump, fail_mute, hw_muted;
static bool fail_end;
static bool fail_stereo;
static tef6686_op_state_t mock_state=TEF6686_STATE_FM;
static tef6686_rds_t next_rds;
static radio_cmd_t queued[8];
static unsigned nq;
int xQueueSend(QueueHandle_t q,const void *p,TickType_t t) { if(nq==8)return 0;queued[nq++]=*(const radio_cmd_t *)p;return 1; }
esp_err_t tef6686_command(tef6686_handle_t d,uint8_t m,uint8_t c,const uint16_t *p,size_t n){return ESP_OK;}
esp_err_t tef6686_tune(tef6686_handle_t d,tef6686_band_t b,tef6686_tune_mode_t m,uint16_t f){
    if(m==TEF6686_TUNE_JUMP && fail_jump){fail_jump=false;return ESP_FAIL;}
    if(m==TEF6686_TUNE_END && fail_end){fail_end=false;return ESP_FAIL;}
    if(m!=TEF6686_TUNE_END)hw_khz=b==TEF6686_BAND_FM?f*10:f;
    hw_mode=m;return ESP_OK;
}
esp_err_t tef6686_fm_get_rds(tef6686_handle_t d,tef6686_rds_t *r){*r=next_rds;next_rds.status=0;return ESP_OK;}
esp_err_t tef6686_get_quality(tef6686_handle_t d,tef6686_band_t b,tef6686_quality_t *q){memset(q,0,sizeof(*q));q->status=1000;q->level=500;return ESP_OK;}
esp_err_t tef6686_get_quality_data(tef6686_handle_t d,tef6686_band_t b,tef6686_quality_t *q){return tef6686_get_quality(d,b,q);}
esp_err_t tef6686_fm_get_stereo(tef6686_handle_t d,bool *v){if(fail_stereo)return ESP_FAIL;*v=true;return ESP_OK;}
esp_err_t tef6686_fm_get_processing(tef6686_handle_t d,tef6686_processing_t *v){memset(v,0,sizeof(*v));return ESP_OK;}
esp_err_t tef6686_set_bandwidth(tef6686_handle_t d,tef6686_band_t b,bool a,uint16_t v,uint16_t c,uint16_t l){return ESP_OK;}
esp_err_t tef6686_fm_set_deemphasis(tef6686_handle_t d,uint16_t t){return ESP_OK;}
esp_err_t tef6686_fm_set_rds(tef6686_handle_t d,bool e){return ESP_OK;}
esp_err_t tef6686_set_volume(tef6686_handle_t d,int16_t v){return ESP_OK;}
esp_err_t tef6686_set_mute(tef6686_handle_t d,bool v){if(fail_mute){fail_mute=false;return ESP_FAIL;}hw_muted=v;return ESP_OK;}
esp_err_t tef6686_audio_enable_dac(tef6686_handle_t d){return ESP_OK;}
esp_err_t tef6686_boot(tef6686_handle_t d){return ESP_OK;}
esp_err_t tef6686_get_identification(tef6686_handle_t d,tef6686_ident_t *v){memset(v,0,sizeof(*v));return ESP_OK;}
esp_err_t tef6686_get_operation_state(tef6686_handle_t d,tef6686_op_state_t *v){*v=mock_state;return ESP_OK;}
const char *tef6686_patch_name(tef6686_handle_t d){return "mock";}
const char *tef6686_variant_name(uint16_t d){return "mock";}
esp_err_t tef6686_new(const tef6686_config_t *c,tef6686_handle_t *d){*d=(void *)1;return ESP_OK;}

static void init(void){
    memset(&s,0,sizeof(s));s.lock=(void *)1;s.post_lock=(void *)1;s.cmds=(void *)1;
    tuning_defaults(&s.tuning);s.status.tef_ready=true;s.status.freq_khz=90000;
    s.status.q.valid=true;s.status.q.age=1000;s.status.op_state=TEF6686_STATE_FM;
    s.status.bw_auto=true;s.status.bw=2360;
    s.cfg.fm_min_khz=87500;s.cfg.fm_max_khz=108000;s.cfg.fm_step_khz=100;s.cfg.am_step_khz=9;
    s.af.origin=90000;hw_khz=90000;hw_mode=TEF6686_TUNE_PRESET;
    mock_tick=1000;nq=0;memset(&next_rds,0,sizeof(next_rds));
    mock_state=TEF6686_STATE_FM;fail_jump=fail_mute=fail_stereo=fail_end=false;
    s.last_quality=mock_tick;
    s.measured=true;
    af_reset(&s.af.st,90000,mock_tick);
}
int main(void){
    init();s.af.verifying=true;s.af.cand=95000;hw_khz=95000;hw_mode=TEF6686_TUNE_CHECK;fail_jump=true;
    af_abort();
    assert(!s.status.tef_ready && hw_khz==95000 && s.status.freq_khz==90000);
    boot_tef();
    assert(s.status.tef_ready && hw_khz==90000 && hw_mode==TEF6686_TUNE_PRESET);
    puts("PASS failed AF return enters recovery and restores original station");

    init();s.af.verifying=true;s.af.cand=95000;fail_end=true;
    af_accept();
    assert(!s.status.tef_ready && !s.af.st.trial);
    puts("PASS failed AF End does not accept a muted candidate");

    init();hw_muted=true;fail_mute=true;
    handle_cmd(&(radio_cmd_t){.type=CMD_MUTE,.mute=false});
    assert(!s.status.tef_ready && hw_muted);
    boot_tef();
    assert(!s.status.mute && !hw_muted && s.status.tef_ready);
    puts("PASS failed unmute is reapplied by recovery");

    init();s.af.pi=0x1234;s.af.pi_known=true;s.af.pi_mem=false;
    next_rds=(tef6686_rds_t){.status=TEF6686_RDS_DATA_AVAILABLE|TEF6686_RDS_SYNC,.block={0x1234,0,0x0102,0x4142}};
    tune_to(RADIO_BAND_FM,95000,false,true);poll_rds();
    uint16_t pi=0;assert(!af_mem_lookup(&s.mem,95000,&pi) && !s.status.rds.pi_valid);
    next_rds=(tef6686_rds_t){.status=TEF6686_RDS_DATA_AVAILABLE|TEF6686_RDS_SYNC,.block={0x5678,0,0x0102,0x4344}};
    poll_rds();
    assert(af_mem_lookup(&s.mem,95000,&pi) && pi==0x5678);
    puts("PASS normal retune drains stale RDS before learning the new PI");

    init();s.af.pi=0xD412;s.af.pi_known=true;s.tuning.v[TUNING_AF_REG]=0;
    s.af.st.trial=true;s.af.st.trial_khz=95000;s.status.freq_khz=95000;
    s.status.rds.pi_valid=true;s.status.rds.pi=0xD512;
    assert(af_pi_match(0xD412,0xD512,false));af_follow_rds();
    assert(s.af.st.trial && s.af.pi==0xD412);
    puts("PASS REG off preserves trial ownership for matching regional PI");

    init();tuning_t a=s.tuning,b=s.tuning;
    a.v[TUNING_AF_GAIN]=10;b.v[TUNING_AF_GAIN]=20;
    radio_set_tuning_all(&a);radio_set_tuning_all(&b);handle_cmd(&queued[0]);
    assert(s.tuning.v[TUNING_AF_GAIN]==10);
    handle_cmd(&queued[1]);assert(s.tuning.v[TUNING_AF_GAIN]==20);
    puts("PASS queued tuning tables retain distinct payloads and order");

    init();s.af.pi=0x1234;s.af.pi_known=true;
    af_mem_learn(&s.mem,0x1234,87600,NULL,0);
    const uint16_t af_group[4]={0x1234,0,0x01CD,0x4142},other[4]={0x1234,0x2000,0,0};
    const uint8_t clean[4]={0};
    rds_process_group(&s.status.rds,af_group,clean);af_follow_rds();
    const uint32_t listed=s.af.st.e[0].listed_ms;
    assert(af_entry_trusted(&s.af.st.e[0],s.af.st.lists,mock_tick));
    mock_tick+=AF_TRUST_MS+1;
    rds_process_group(&s.status.rds,other,clean);af_follow_rds();
    assert(s.af.st.e[0].listed_ms==listed);
    assert(!af_entry_trusted(&s.af.st.e[0],s.af.st.lists,mock_tick));
    puts("PASS non-AF RDS groups do not renew stale AF trust");

    init();
    assert(radio_tune(RADIO_BAND_AM,999)==ESP_OK);
    assert(radio_set_bandwidth(false,2360)==ESP_OK);
    handle_cmd(&queued[0]);handle_cmd(&queued[1]);
    assert(s.status.band==RADIO_BAND_AM && s.status.bw==40);
    radio_settings_t settings;radio_get_settings(&settings);
    assert(radio_settings_valid(&settings));
    puts("PASS bandwidth queued for another band cannot corrupt active settings");

    init();s.status.tef_ready=false;s.status.bw=2360;
    handle_cmd(&(radio_cmd_t){.type=CMD_TUNE,.tune={.band=RADIO_BAND_AM,.khz=1000}});
    assert(s.status.band==RADIO_BAND_AM && s.status.bw==40);
    mock_state=TEF6686_STATE_AM;boot_tef();assert(s.status.tef_ready && hw_khz==1000);
    puts("PASS offline band changes retain valid bandwidth through recovery");

    init();mock_state=TEF6686_STATE_STANDBY;check_health();
    assert(!s.status.tef_ready && s.status.op_state==TEF6686_STATE_STANDBY);
    puts("PASS unexpected standby triggers recovery");

    init();fail_stereo=true;
    for(unsigned i=0;i<100 && s.status.tef_ready;i++){poll_quality();mock_tick+=100;}
    assert(s.io_errors>=MAX_IO_ERRORS && !s.status.tef_ready);
    fail_stereo=false;
    puts("PASS persistent stereo failure is not hidden by successful quality reads");

    init();s.cfg.am_step_khz=10;s.status.band=RADIO_BAND_AM;s.status.freq_khz=1710;
    step(1);
    assert(s.status.freq_khz==530);
    step(-1);assert(s.status.freq_khz==1710);
    s.status.freq_khz=999;step(1);assert(s.status.freq_khz==1000);
    puts("PASS 10 kHz MW wrap and off-grid manual frequency follow the regional raster");

    init();seek_begin(1,true,0,0,100,band_range(RADIO_BAND_FM,90000));
    mock_tick=s.seek.deadline;seek_poll();assert(!s.seek.active);
    puts("PASS seek has an overall deadline");

    init();assert(radio_diagnostics_passed());
    mock_tick+=1000;assert(!radio_diagnostics_passed());
    init();s.status.tef_ready=false;assert(!radio_diagnostics_passed());
    puts("PASS OTA diagnostics reject stale quality and unavailable tuner");

    init();assert(radio_set_volume(-100)==ESP_OK);
    assert(radio_wait_commands(10)==ESP_ERR_TIMEOUT);
    handle_cmd(&queued[0]);s.completed=queued[0].seq;
    assert(radio_wait_commands(10)==ESP_OK);
    puts("PASS save barrier waits for accepted command completion");
    return 0;
}
