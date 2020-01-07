#include "mruby.h"
#include <Arduino.h>
#ifdef __cplusplus
extern "C" {
#endif


#include <mruby.h>
#include <mruby/variable.h>
#include <mruby/dump.h>
#include <mruby/irep.h>
#include <mruby/string.h>
#include <mruby/proc.h>

void mrb_codedump_all(mrb_state*, struct RProc*);

inline bool waitForReadAvailable(int waitMs = 1000){
  int start_ms = millis();
  while( (millis() - start_ms) < waitMs){
    if (Serial.available() > 0) {
      return true;
    }
  }
  return false;
}

bool readByteCode(byte *buffer, int max_len, int *len, int *verbose){
  byte soh = Serial.read();
  if ((soh == 0x01 || soh == 0x02)) {
  
  }else if (soh == 0x05) { // ENQ
    //send ACK 
    Serial.write((byte)0x06);
    return false;
  } else {
    //something wrong!!! 
    Serial.println("(target):NON SOH received as start of data");
    return false;    
  }
  *verbose = (soh == 0x02) ? 1 : 0;

  if (!waitForReadAvailable()) return false;
  unsigned short lengthH = Serial.read();
  if (!waitForReadAvailable()) return false;
  unsigned short lengthL = Serial.read();
  unsigned short lenToRead = (lengthH << 8 | lengthL);

  if(lenToRead > max_len){    
    Serial.println("bytecode exceeds buffer size");
  }
  Serial.write('!');

  unsigned short lenReaded = 0;
  while(lenReaded < lenToRead){
    for (int i = 0 ; i < 100 ; i++){
      if (!waitForReadAvailable()) return false;
      buffer[lenReaded] = Serial.read();
      lenReaded++;
      if (lenReaded == lenToRead){
        break;
      }
    }
    Serial.write('#');
  }

  *len = lenReaded;
  return true;
}

//accept non-null-terminated string
void writeResult(const char *result, size_t len, int isException){
  unsigned short lenToWrite = (unsigned short)len;
  byte lengthH = (byte)(lenToWrite >> 8);
  byte lengthL = (byte)(lenToWrite & 0xFF);

  const byte SOH = isException? 0x02 : 0x01;

  Serial.write(SOH);
  Serial.write(lengthH);
  Serial.write(lengthL);
  if (!waitForReadAvailable()) return;
  Serial.read();      //must be a '!'

  unsigned short lenWritten = 0;
  while(lenWritten < lenToWrite){
    for (int i = 0 ; i < 100 ;i++){
      Serial.write((byte)result[lenWritten]);
      lenWritten++;
      if (lenWritten == lenToWrite){
        break;
      }
    }
    if (!waitForReadAvailable()) return;
    Serial.read();    //must be a '#'
  }
}

void writeResultStr(const char *resultStr, int isException){
  writeResult(resultStr, strlen(resultStr), isException);
}

byte g_byteCodeBuf[2048];

void readEvalPrint(mrb_state *mrb, mrb_value self){
  int verbose = 0;

  //receive bytecode
  int byteCodeLen = 0;
  if (!readByteCode( g_byteCodeBuf, sizeof(g_byteCodeBuf), &byteCodeLen, &verbose))
    return;


  //DPRINTF("readByteCode done\n",0);

  //load bytecode
  struct mrb_irep *irep = mrb_read_irep_buf(mrb, g_byteCodeBuf, byteCodeLen);
  if (!irep) {
    const char *resultStr = "(target):illegal bytecode.";
    writeResultStr(resultStr, 1);
    return;
  }

  //DPRINTF("mirb_read_ire_file done.\n");

  struct RProc *proc;
  proc = mrb_proc_new(mrb, irep);
  mrb_irep_decref(mrb, irep);
  if (verbose) mrb_codedump_all(mrb, proc);

  //evaluate the bytecode
  mrb_value result;
  
  int nregs = 0;
  if(mrb_test(mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "first_command")))){
    mrb_obj_iv_set(mrb, mrb_obj_ptr(self), mrb_intern_lit(mrb, "@first_command"), mrb_false_value());
  }else{
    nregs = proc->body.irep->nregs;
  }

  result = mrb_context_run(mrb, proc, mrb_top_self(mrb), nregs);

  //DPRINTF("mrb_run done. exception = %d\n", (int)mrb->exc);

  //prepare inspected string from result
  mrb_value result_str;
  int exeption_on_run = mrb->exc? 1: 0;
  if (exeption_on_run){
    result_str = mrb_funcall(mrb, mrb_obj_value(mrb->exc),"inspect",0);
    mrb->exc = 0;
  }else{
    //DPRINTF("asking #inspect possibility to result\n");
    if (!mrb_respond_to(mrb, result, mrb_intern_cstr(mrb, "inspect"))){
      result_str = mrb_any_to_s(mrb, result);
    }else{
      result_str = mrb_funcall(mrb, result, "inspect",0);
    }
    mrb_gc_mark_value(mrb, result);
  }

  if (mrb->exc == 0) {
    //DPRINTF("inspected result:%s\n", mrb_str_to_cstr(mrb, result_str));
  }

  //write result string back to host
  if (mrb->exc){  //failed to inspect
    mrb->exc = 0;
    const char *msg = "(target):too low memory to return anything.";
    writeResultStr(msg, 1);
  }else{
    writeResult(RSTRING_PTR(result_str), RSTRING_LEN(result_str), exeption_on_run);
    mrb_gc_mark_value(mrb, result_str);
  }
}

mrb_value mrb_mirb_server_stop(mrb_state *mrb, mrb_value self) {
  mrb_obj_iv_set(mrb, mrb_obj_ptr(self), mrb_intern_lit(mrb, "@running"), mrb_false_value());
  return self;
}


mrb_value mrb_mirb_server_run(mrb_state *mrb, mrb_value self) {
  int ai = mrb_gc_arena_save(mrb);
  mrb_sym ivarname = mrb_intern_lit(mrb, "@running");
  mrb_value running = mrb_true_value();
  Serial.begin(9600);

  mrb_obj_iv_set(mrb, mrb_obj_ptr(self), ivarname, mrb_true_value());
  mrb_obj_iv_set(mrb, mrb_obj_ptr(self), mrb_intern_lit(mrb, "@first_command"), mrb_true_value());

  do {
    if (Serial.available() > 0 ){
      readEvalPrint(mrb, self);
      mrb_gc_arena_restore(mrb, ai); 
    }
    delay(10);
    running = mrb_obj_iv_get(mrb, mrb_obj_ptr(self), ivarname);

  }while(mrb_test(running));

  return self;
}






void
mrb_mruby_mirb_server_gem_init(mrb_state* mrb)
{
  struct RClass *MirbServer  = mrb_define_class(mrb, "Mirb",  mrb->object_class);

  mrb_define_class_method(mrb, MirbServer, "run", mrb_mirb_server_run, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, MirbServer, "stop", mrb_mirb_server_stop, MRB_ARGS_NONE());
  
}

void
mrb_mruby_mirb_server_gem_final(mrb_state* mrb)
{
}

#ifdef __cplusplus
}
#endif
