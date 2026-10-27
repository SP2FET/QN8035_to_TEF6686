#pragma once
#define ESP_RETURN_ON_FALSE(c,e,...) do { if (!(c)) return (e); } while(0)
#define ESP_RETURN_ON_ERROR(c,...) do { int e_=(c); if(e_)return e_; } while(0)
