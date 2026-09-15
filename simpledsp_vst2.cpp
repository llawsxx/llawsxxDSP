#include "audioeffectx.h"
#include "convolution_reverb.h"
#include "simpledsp_editor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr int Bands=4, WaveSize=16384, SubN=30, GateN=300, LraN=30;
constexpr double Pi=3.14159265358979323846;
enum Param { EqOn, E1F,E1G,E1Q,E2F,E2G,E2Q,E3F,E3G,E3Q,E4F,E4G,E4Q,
 RevOn,Room,Decay,Damp,Mix,LoudOn,Target,Lra,TruePeak,LimOn,LimInput,Limit,
 Release,Ceiling,Lookahead,Adaptive,ParamCount };
const char* names[ParamCount]={"EQ On","EQ1 Freq","EQ1 Gain","EQ1 Q","EQ2 Freq","EQ2 Gain","EQ2 Q",
 "EQ3 Freq","EQ3 Gain","EQ3 Q","EQ4 Freq","EQ4 Gain","EQ4 Q","Rev On","Room","Decay","Damping",
 "Mix","Loud On","Target","LRA","TruePk","Lim On","Input","Limit","Release","Ceiling","Lookahead","Adaptive"};
float cl(float x,float a,float b){return std::max(a,std::min(b,x));}
double cld(double x,double a,double b){return std::max(a,std::min(b,x));}
float lin(float n,float a,float b){return a+cl(n,0.f,1.f)*(b-a);}
float logmap(float n,float a,float b){return a*std::pow(b/a,cl(n,0.f,1.f));}
float invlog(float x,float a,float b){return std::log(cl(x,a,b)/a)/std::log(b/a);}
float db2lin(float x){return std::pow(10.f,x/20.f);}
float amp2db(float x){return 20.f*std::log10(std::max(x,1e-6f));}
bool toggle(int i){return i==EqOn||i==RevOn||i==LoudOn||i==LimOn||i==Adaptive;}

struct Biquad{
 float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0,lf=-1,lg=0,lq=0;
 void config(int sr,float f,float g,float q){f=cl(f,20.f,sr*.49f);q=cl(q,.1f,20.f);if(f==lf&&g==lg&&q==lq)return;lf=f;lg=g;lq=q;
  float A=std::pow(10.f,g/40.f),w=2.f*(float)Pi*f/sr,alpha=std::sin(w)/(2*q),c=std::cos(w),d=1+alpha/A;
  b0=(1+alpha*A)/d;b1=-2*c/d;b2=(1-alpha*A)/d;a1=-2*c/d;a2=(1-alpha/A)/d;}
 float run(float x){float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
 void reset(){z1=z2=0;}
};

struct Loudness{
 int rate=48000,subFrames=4800,subCount=0,subPos=0,subValid=0,gatePos=0,gateValid=0,lraPos=0,lraValid=0,lraHop=0;
 double b[5]{},a[5]{},v[2][5]{},subs[SubN]{},gates[GateN]{},lras[LraN]{},subSum=0,measuredLra=0;
 double gain=1,wanted=1,gainCoef=0,peakGain=1,peakRelease=0,tpLimit=1,tpDb=999;
 static double energy(double l){return std::pow(10.,(l+.691)/10.);}
 static double loud(double e){return 10.*std::log10(std::max(e,1e-12))-.691;}
 static double recent(const double*x,int n,int next,int count){double s=0;count=std::min(n,count);for(int i=1;i<=count;i++){int p=next-i;if(p<0)p+=n;s+=x[p];}return count?s/count:0;}
 void init(int sr){rate=std::max(sr,8000);double f=1681.974450955533,g=3.999843853973347,q=.7071752369554196,k=std::tan(Pi*f/rate),vh=std::pow(10.,g/20.),vb=std::pow(vh,.4996667741545416);
  double pb[3],pa[3]={1,0,0},rb[3]={1,-2,1},ra[3]={1,0,0},d=1+k/q+k*k;pb[0]=(vh+vb*k/q+k*k)/d;pb[1]=2*(k*k-vh)/d;pb[2]=(vh-vb*k/q+k*k)/d;pa[1]=2*(k*k-1)/d;pa[2]=(1-k/q+k*k)/d;
  f=38.13547087602444;q=.5003270373238773;k=std::tan(Pi*f/rate);d=1+k/q+k*k;ra[1]=2*(k*k-1)/d;ra[2]=(1-k/q+k*k)/d;
  b[0]=pb[0]*rb[0];b[1]=pb[0]*rb[1]+pb[1]*rb[0];b[2]=pb[0]*rb[2]+pb[1]*rb[1]+pb[2]*rb[0];b[3]=pb[1]*rb[2]+pb[2]*rb[1];b[4]=pb[2]*rb[2];
  a[0]=1;a[1]=ra[1]+pa[1];a[2]=ra[2]+pa[1]*ra[1]+pa[2];a[3]=pa[1]*ra[2]+pa[2]*ra[1];a[4]=pa[2]*ra[2];subFrames=std::max(1,(rate+5)/10);peakRelease=1-std::exp(-1./(rate*.1));reset();}
 void reset(){std::memset(v,0,sizeof(v));std::memset(subs,0,sizeof(subs));std::memset(gates,0,sizeof(gates));std::memset(lras,0,sizeof(lras));subCount=subPos=subValid=gatePos=gateValid=lraPos=lraValid=lraHop=0;subSum=measuredLra=0;gain=wanted=peakGain=tpLimit=1;gainCoef=0;tpDb=999;}
 double filter(int c,double x){double*s=v[c];s[0]=x-a[1]*s[1]-a[2]*s[2]-a[3]*s[3]-a[4]*s[4];double y=b[0]*s[0]+b[1]*s[1]+b[2]*s[2]+b[3]*s[3]+b[4]*s[4];s[4]=s[3];s[3]=s[2];s[2]=s[1];s[1]=s[0];return y;}
 void updateLra(double e){if(++lraHop<10)return;lraHop=0;lras[lraPos]=e;lraPos=(lraPos+1)%LraN;lraValid=std::min(lraValid+1,LraN);double ag=energy(-70),sum=0;int n=0;for(int i=0;i<lraValid;i++)if(lras[i]>=ag){sum+=lras[i];n++;}if(n<4)return;double gate=std::max(ag,sum/n*.01),sorted[LraN];int sn=0;for(int i=0;i<lraValid;i++){if(lras[i]<gate)continue;double x=loud(lras[i]);int at=sn;while(at&&sorted[at-1]>x){sorted[at]=sorted[at-1];at--;}sorted[at]=x;sn++;}if(sn>=4)measuredLra=sorted[(int)std::ceil(.95*(sn-1))]-sorted[(int)std::floor(.10*(sn-1))];}
 void target(float target,float targetLra){if(subValid<4)return;gates[gatePos]=recent(subs,SubN,subPos,4);gatePos=(gatePos+1)%GateN;gateValid=std::min(gateValid+1,GateN);double ag=energy(-70),sum=0;int n=0;for(int i=0;i<gateValid;i++)if(gates[i]>=ag){sum+=gates[i];n++;}if(!n)return;double gate=std::max(ag,sum/n*.1),gs=0;int gn=0;for(int i=0;i<gateValid;i++)if(gates[i]>=gate){gs+=gates[i];gn++;}if(!gn)return;double integrated=loud(gs/gn);int count=std::min(subValid,SubN);double se=recent(subs,SubN,subPos,count),st=loud(se);if(st<-60)return;if(count==SubN)updateLra(se);double corr=measuredLra>targetLra?(st-integrated)*(targetLra/measuredLra-1):0;double gd=cld(target-integrated+cld(corr,-6,6),-12,18);wanted=std::pow(10.,gd/20.);double tau=wanted<gain?.08:std::min(1.5,.25+.035*targetLra);gainCoef=1-std::exp(-1./(rate*tau));}
 void run(float&l,float&r,float targetDb,float targetLra,float trueDb){if(trueDb!=tpDb){tpDb=trueDb;tpLimit=std::pow(10.,tpDb/20.);}double wl=filter(0,l),wr=filter(1,r);subSum+=wl*wl+wr*wr;if(++subCount>=subFrames){subs[subPos]=subSum/subCount;subPos=(subPos+1)%SubN;subValid=std::min(subValid+1,SubN);subSum=0;subCount=0;target(targetDb,targetLra);}gain+= (wanted-gain)*gainCoef;gain=cld(gain,.0630957,7.94328);double peak=std::max(std::fabs(l),std::fabs(r)),applied=gain*peakGain;if(peak>1e-9&&peak*applied>tpLimit)peakGain=std::min(peakGain,tpLimit/(peak*gain));else peakGain+=(1-peakGain)*peakRelease;peakGain=cld(peakGain,0,1);applied=gain*peakGain;l=(float)(l*applied);r=(float)(r*applied);}
};

struct Limiter{
 int rate=48000,maxDelay=240,write=0,filled=0,lastDelay=0;float envelope=1,activity=0;std::vector<float> l,r,p;
 void init(int sr){rate=std::max(sr,8000);maxDelay=std::max(1,(int)std::ceil(rate*.005));l.assign(maxDelay+1,0);r.assign(maxDelay+1,0);p.assign(maxDelay+1,0);reset();}
 void reset(){std::fill(l.begin(),l.end(),0.f);std::fill(r.begin(),r.end(),0.f);std::fill(p.begin(),p.end(),0.f);write=filled=lastDelay=0;envelope=1;activity=0;}
 void run(float&x,float&y,float inputDb,float limitDb,float releaseMs,float ceilingDb,float lookMs,bool adaptive){int cap=(int)l.size(),delay=std::max(1,std::min(maxDelay,(int)(lookMs*rate/1000)));if(lastDelay&&lastDelay!=delay)reset();lastDelay=delay;float ig=db2lin(inputDb),limit=std::min(db2lin(limitDb),db2lin(ceilingDb)),ceiling=db2lin(ceilingDb);l[write]=x*ig;r[write]=y*ig;p[write]=std::max(std::fabs(l[write]),std::fabs(r[write]));write=(write+1)%cap;filled=std::min(filled+1,cap);float future=0;for(int i=1;i<=std::min(filled,delay+1);i++){int at=write-i;if(at<0)at+=cap;future=std::max(future,p[at]);}float wanted=future>limit?limit/future:1;if(adaptive){if(future>limit)activity+=(1-activity)/(rate*.15f);else activity-=activity/(rate*.4f);activity=cl(activity,0,1);}else activity=0;float rel=cl(releaseMs*(adaptive?.5f+1.5f*activity:1)/1000,.01f,10.f);if(wanted<envelope)envelope=wanted;else envelope+=(1-envelope)*(1-std::exp(-1.f/(rate*rel)));int out=write-delay-1;while(out<0)out+=cap;if(filled<=delay){x=y=0;return;}x=cl(l[out]*envelope,-ceiling,ceiling);y=cl(r[out]*envelope,-ceiling,ceiling);}
};
}

class SimpleDSP final:public AudioEffectX,public SimpleDSPUiSource{
public:
 explicit SimpleDSP(audioMasterCallback cb):AudioEffectX(cb,1,ParamCount){setNumInputs(2);setNumOutputs(2);canProcessReplacing();setUniqueID(CCONST('l','D','S','P'));vst_strncpy(program,"Default",kVstMaxProgNameLen);for(auto&x:param)x.store(0);defaults();for(auto&x:waveIn)x.store(0);for(auto&x:waveOut)x.store(0);loud.init(rate);limiter.init(rate);setEditor(createSimpleDSPEditor(this,this));worker=std::thread(&SimpleDSP::reverbWorker,this);}
 ~SimpleDSP()override{{std::lock_guard<std::mutex>g(workMutex);stopping=true;requested=true;}condition.notify_one();if(worker.joinable())worker.join();std::lock_guard<std::mutex>g(reverbMutex);convolution_reverb_destroy(reverb);}
 void defaults(){param[EqOn]=0;float f[4]={100,500,2000,8000};for(int i=0;i<4;i++){param[E1F+i*3]=invlog(f[i],20,20000);param[E1G+i*3]=.5f;param[E1Q+i*3]=invlog(1,.1f,20);}param[RevOn]=0;param[Room]=.5f;param[Decay]=(1.5f-.2f)/5.8f;param[Damp]=.5f;param[Mix]=.25f;param[LoudOn]=0;param[Target]=56.f/65.f;param[Lra]=10.f/49.f;param[TruePeak]=8.f/9.f;param[LimOn]=0;param[LimInput]=1.f/3.f;param[Limit]=.95f;param[Release]=invlog(100,10,1000);param[Ceiling]=11.f/12.f;param[Lookahead]=1;param[Adaptive]=1;}
 void resume()override{rate=std::max(8000,(int)getSampleRate());for(auto&c:eq)for(auto&b:c)b.reset();loud.init(rate);limiter.init(rate);resetMeters();latency();requestReverb();AudioEffectX::resume();}
 void suspend()override{AudioEffectX::suspend();loud.reset();limiter.reset();resetMeters();}
 void setParameter(VstInt32 i,float x)override{if(i<0||i>=ParamCount)return;param[i].store(cl(x,0,1));if(i==RevOn||i==Room||i==Decay||i==Damp)requestReverb();if(i==Lookahead||i==LimOn)latency();}
 float getParameter(VstInt32 i)override{return i>=0&&i<ParamCount?param[i].load():0;}
 float actual(int i)const{float x=i>=0&&i<ParamCount?param[i].load():0;if(toggle(i))return x>=.5f;if(i==E1F||i==E2F||i==E3F||i==E4F)return logmap(x,20,20000);if(i==E1G||i==E2G||i==E3G||i==E4G)return lin(x,-18,18);if(i==E1Q||i==E2Q||i==E3Q||i==E4Q)return logmap(x,.1f,20);if(i==Room||i==Damp||i==Mix)return lin(x,0,100);if(i==Decay)return lin(x,.2f,6);if(i==Target)return lin(x,-70,-5);if(i==Lra)return lin(x,1,50);if(i==TruePeak)return lin(x,-9,0);if(i==LimInput)return lin(x,-12,24);if(i==Limit)return lin(x,-20,0);if(i==Release)return logmap(x,10,1000);if(i==Ceiling)return lin(x,-12,0);if(i==Lookahead)return lin(x,.1f,5);return x;}
 void getParameterName(VstInt32 i,char*s)override{vst_strncpy(s,i>=0&&i<ParamCount?names[i]:"",kVstMaxParamStrLen);}
 void getParameterDisplay(VstInt32 i,char*s)override{format(i,s,kVstMaxParamStrLen);}
 void getParameterLabel(VstInt32 i,char*s)override{const char*u="";if(i==E1F||i==E2F||i==E3F||i==E4F)u="Hz";else if(i==E1G||i==E2G||i==E3G||i==E4G||i==Target||i==TruePeak||i==LimInput||i==Limit||i==Ceiling)u="dB";else if(i==Room||i==Damp||i==Mix)u="%";else if(i==Decay)u="s";else if(i==Release||i==Lookahead)u="ms";else if(i==Lra)u="LU";vst_strncpy(s,u,kVstMaxParamStrLen);}
 void setProgramName(char*n)override{vst_strncpy(program,n,kVstMaxProgNameLen);}void getProgramName(char*n)override{vst_strncpy(n,program,kVstMaxProgNameLen);}bool getEffectName(char*n)override{vst_strncpy(n,"llawsxxDSP",kVstMaxEffectNameLen);return true;}bool getProductString(char*n)override{vst_strncpy(n,"llawsxxDSP",kVstMaxProductStrLen);return true;}bool getVendorString(char*n)override{vst_strncpy(n,"llawsxx",kVstMaxVendorStrLen);return true;}VstInt32 getVendorVersion()override{return 1000;}VstPlugCategory getPlugCategory()override{return kPlugCategEffect;}VstInt32 canDo(char*s)override{return !std::strcmp(s,"plugAsChannelInsert")||!std::strcmp(s,"plugAsSend")||!std::strcmp(s,"x2in2out")?1:-1;}float getVu(){return meterOutPub.load();}
 void processReplacing(float**in,float**out,VstInt32 n)override{if(!in||!out||!in[0]||!in[1]||!out[0]||!out[1])return;float p[ParamCount];for(int i=0;i<ParamCount;i++)p[i]=param[i].load();bool eqOn=p[EqOn]>=.5f,revOn=p[RevOn]>=.5f,loudOn=p[LoudOn]>=.5f,limOn=p[LimOn]>=.5f;if(!loudOn&&wasLoud)loud.reset();if(!limOn&&wasLim)limiter.reset();if(eqOn)for(int b=0;b<Bands;b++){float f=logmap(p[E1F+b*3],20,20000),g=lin(p[E1G+b*3],-18,18),q=logmap(p[E1Q+b*3],.1f,20);eq[0][b].config(rate,f,g,q);eq[1][b].config(rate,f,g,q);}std::unique_lock<std::mutex>rvlock(reverbMutex,std::defer_lock);if(revOn)rvlock.lock();float mr=std::exp(-1.f/(rate*.3f)),hr=std::exp(-1.f/(rate*.5f));for(int i=0;i<n;i++){float l=in[0][i],r=in[1][i];meter(std::max(std::fabs(l),std::fabs(r)),meterIn,heldIn,holdIn,mr,hr);unsigned w=waveWrite.load();waveIn[w]=.5f*(l+r);if(eqOn)for(int b=0;b<4;b++){l=eq[0][b].run(l);r=eq[1][b].run(r);}if(revOn&&reverb){float wl=0,wr=0;convolution_reverb_process(reverb,l,r,&wl,&wr);l=l*(1-p[Mix])+wl*p[Mix];r=r*(1-p[Mix])+wr*p[Mix];}if(loudOn)loud.run(l,r,lin(p[Target],-70,-5),lin(p[Lra],1,50),lin(p[TruePeak],-9,0));if(limOn)limiter.run(l,r,lin(p[LimInput],-12,24),lin(p[Limit],-20,0),logmap(p[Release],10,1000),lin(p[Ceiling],-12,0),lin(p[Lookahead],.1f,5),p[Adaptive]>=.5f);out[0][i]=l;out[1][i]=r;meter(std::max(std::fabs(l),std::fabs(r)),meterOut,heldOut,holdOut,mr,hr);waveOut[w]=.5f*(l+r);waveWrite.store((w+1)&(WaveSize-1),std::memory_order_release);}wasLoud=loudOn;wasLim=limOn;meterInPub=meterIn;meterOutPub=meterOut;heldInPub=heldIn;heldOutPub=heldOut;}
 float uiParameter(int i)const override{return i>=0&&i<ParamCount?param[i].load():0;}
 void uiSetParameter(int i,float x)override{setParameterAutomated(i,x);}
 void uiBeginEdit(int i)override{beginEdit(i);}
 void uiEndEdit(int i)override{endEdit(i);}
 void uiParameterText(int i,char*s,int z)const override{format(i,s,z);}
 void uiSetParameterText(int i,const char*s)override{
  if(i<0||i>=ParamCount||!s)return;
  char*end=nullptr;float v=(float)std::strtod(s,&end);if(end==s)return;float n=v;
  if(toggle(i))n=(v!=0.f||s[0]=='o'||s[0]=='O')?1.f:0.f;
  else if(i==E1F||i==E2F||i==E3F||i==E4F)n=invlog(v,20,20000);
  else if(i==E1G||i==E2G||i==E3G||i==E4G)n=(v+18)/36;
  else if(i==E1Q||i==E2Q||i==E3Q||i==E4Q)n=invlog(v,.1f,20);
  else if(i==Room||i==Damp||i==Mix)n=v/100;
  else if(i==Decay)n=(v-.2f)/5.8f;
  else if(i==Target)n=(v+70)/65;
  else if(i==Lra)n=(v-1)/49;
  else if(i==TruePeak)n=(v+9)/9;
  else if(i==LimInput)n=(v+12)/36;
  else if(i==Limit)n=(v+20)/20;
  else if(i==Release)n=invlog(v,10,1000);
  else if(i==Ceiling)n=(v+12)/12;
  else if(i==Lookahead)n=(v-.1f)/4.9f;
  setParameterAutomated(i,cl(n,0,1));
 }
 void uiCopyWave(int count,float*in,float*out)const override{count=std::max(1,std::min(count,WaveSize));unsigned end=waveWrite.load(std::memory_order_acquire),start=(end+WaveSize-count)&(WaveSize-1);for(int i=0;i<count;i++){unsigned at=(start+i)&(WaveSize-1);in[i]=waveIn[at].load();out[i]=waveOut[at].load();}}
 float uiInputDb()const override{return amp2db(meterInPub.load());}
 float uiOutputDb()const override{return amp2db(meterOutPub.load());}
 float uiInputPeakDb()const override{return amp2db(heldInPub.load());}
 float uiOutputPeakDb()const override{return amp2db(heldOutPub.load());}
private:
 void format(int i,char*s,int z)const{if(i<0||i>=ParamCount){*s=0;return;}float x=actual(i);if(toggle(i))std::snprintf(s,z,"%s",x>=.5f?"On":"Off");else if(i==E1F||i==E2F||i==E3F||i==E4F)std::snprintf(s,z,x>=1000?"%.2fk Hz":"%.0f Hz",x>=1000?x/1000:x);else if(i==E1Q||i==E2Q||i==E3Q||i==E4Q)std::snprintf(s,z,"%.2f",x);else if(i==Room||i==Damp||i==Mix)std::snprintf(s,z,"%.0f%%",x);else if(i==Decay)std::snprintf(s,z,"%.2f s",x);else if(i==Release)std::snprintf(s,z,"%.0f ms",x);else if(i==Lookahead)std::snprintf(s,z,"%.2f ms",x);else if(i==Lra)std::snprintf(s,z,"%.1f LU",x);else std::snprintf(s,z,"%.1f dB",x);}
 void requestReverb(){{std::lock_guard<std::mutex>g(workMutex);requested=true;}condition.notify_one();}
 void reverbWorker(){for(;;){{std::unique_lock<std::mutex>g(workMutex);condition.wait(g,[this]{return requested;});if(stopping)return;requested=false;}ConvolutionReverb*x=convolution_reverb_create(rate,actual(Room),actual(Decay),actual(Damp));if(!x)continue;ConvolutionReverb*old=nullptr;{{std::lock_guard<std::mutex>g(reverbMutex);old=reverb;reverb=x;}}convolution_reverb_destroy(old);}}
 void latency(){bool on=param[LimOn].load()>=.5f;setInitialDelay(on?std::max(1,(int)(actual(Lookahead)*rate/1000)):0);updateDisplay();}
 void meter(float p,float&now,float&held,int&frames,float mr,float hr){now=std::max(p,now*mr);if(p>=held){held=p;frames=(int)(rate*1.5f);}else if(frames>0)frames--;else held=std::max(p,held*hr);}
 void resetMeters(){meterIn=meterOut=heldIn=heldOut=0;holdIn=holdOut=0;meterInPub=meterOutPub=heldInPub=heldOutPub=0;}
 std::array<std::atomic<float>,ParamCount>param;Biquad eq[2][Bands];Loudness loud;Limiter limiter;bool wasLoud=false,wasLim=false;int rate=48000;char program[kVstMaxProgNameLen+1]{};
 ConvolutionReverb*reverb=nullptr;std::mutex reverbMutex,workMutex;std::condition_variable condition;std::thread worker;bool requested=false,stopping=false;
 std::array<std::atomic<float>,WaveSize>waveIn,waveOut;std::atomic<unsigned>waveWrite{0};float meterIn=0,meterOut=0,heldIn=0,heldOut=0;int holdIn=0,holdOut=0;std::atomic<float>meterInPub{0},meterOutPub{0},heldInPub{0},heldOutPub{0};
};

AudioEffect* createEffectInstance(audioMasterCallback cb){return new SimpleDSP(cb);}
extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
AEffect* VSTPluginMain(audioMasterCallback cb){if(!cb)return nullptr;AudioEffect*e=createEffectInstance(cb);return e?e->getAeffect():nullptr;}
}
