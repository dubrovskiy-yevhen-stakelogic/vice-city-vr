// Recompresses uncompressed 32-bit textures inside RenderWare D3D8 TXD files
// to DXT1 (opaque) or DXT5 (alpha) with a full mip chain. Textures that are
// already compressed, palettized or tiny are copied through untouched.
//
//   txdcompress <file.txd> [more.txd ...]
//
// Each file is rewritten in place; a report line is printed per texture.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

static u32 rd32(const u8 *p){ u32 v; memcpy(&v,p,4); return v; }
static u16 rd16(const u8 *p){ u16 v; memcpy(&v,p,2); return v; }
static void wr32(std::vector<u8> &o, u32 v){ o.insert(o.end(),(u8*)&v,(u8*)&v+4); }
static void wr16(std::vector<u8> &o, u16 v){ o.insert(o.end(),(u8*)&v,(u8*)&v+2); }

// ---- DXT block compression (bounding-box range fit) ----

static u16 pack565(int r,int g,int b){
	return (u16)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}

// pixels: 16 RGBA entries (row-major 4x4)
static void encodeColorBlock(const u8 px[16][4], u8 out[8], bool opaqueDxt1)
{
	int minc[3]={255,255,255}, maxc[3]={0,0,0};
	for(int i=0;i<16;i++) for(int c=0;c<3;c++){
		if(px[i][c]<minc[c]) minc[c]=px[i][c];
		if(px[i][c]>maxc[c]) maxc[c]=px[i][c];
	}
	// inset to reduce banding
	for(int c=0;c<3;c++){
		int inset=(maxc[c]-minc[c])/16;
		minc[c]+=inset; maxc[c]-=inset;
		if(minc[c]>maxc[c]){ int m=(minc[c]+maxc[c])/2; minc[c]=maxc[c]=m; }
	}
	u16 c0=pack565(maxc[0],maxc[1],maxc[2]);
	u16 c1=pack565(minc[0],minc[1],minc[2]);
	if(opaqueDxt1 && c0==c1 && c0>0) c1--;      // force 4-colour mode
	else if(opaqueDxt1 && c0==c1) c0++;
	if(opaqueDxt1 && c0<c1){ u16 t=c0;c0=c1;c1=t; }
	// palette
	int pal[4][3];
	pal[0][0]=(c0>>11)<<3; pal[0][1]=((c0>>5)&63)<<2; pal[0][2]=(c0&31)<<3;
	pal[1][0]=(c1>>11)<<3; pal[1][1]=((c1>>5)&63)<<2; pal[1][2]=(c1&31)<<3;
	for(int c=0;c<3;c++){
		pal[2][c]=(2*pal[0][c]+pal[1][c])/3;
		pal[3][c]=(pal[0][c]+2*pal[1][c])/3;
	}
	u32 indices=0;
	for(int i=0;i<16;i++){
		int best=0, bestd=1<<30;
		for(int j=0;j<4;j++){
			int d=0;
			for(int c=0;c<3;c++){ int diff=px[i][c]-pal[j][c]; d+=diff*diff; }
			if(d<bestd){ bestd=d; best=j; }
		}
		indices|=(u32)best<<(i*2);
	}
	memcpy(out,&c0,2); memcpy(out+2,&c1,2); memcpy(out+4,&indices,4);
}

static void encodeAlphaBlock(const u8 px[16][4], u8 out[8])
{
	int mina=255, maxa=0;
	for(int i=0;i<16;i++){ if(px[i][3]<mina)mina=px[i][3]; if(px[i][3]>maxa)maxa=px[i][3]; }
	if(mina==maxa){ if(maxa<255)maxa++; else mina--; }
	out[0]=(u8)maxa; out[1]=(u8)mina;   // 8-interp mode (a0 > a1)
	int pal[8];
	pal[0]=maxa; pal[1]=mina;
	for(int j=1;j<7;j++) pal[j+1]=((7-j)*maxa+j*mina)/7;
	u8 idx[16];
	for(int i=0;i<16;i++){
		int best=0,bestd=1<<30;
		for(int j=0;j<8;j++){ int d=px[i][3]-pal[j]; d*=d; if(d<bestd){bestd=d;best=j;} }
		idx[i]=(u8)best;
	}
	// pack 16 3-bit indices into 6 bytes
	uint64_t bits=0;
	for(int i=0;i<16;i++) bits|=(uint64_t)idx[i]<<(i*3);
	for(int i=0;i<6;i++) out[2+i]=(u8)(bits>>(i*8));
}

// rgba: w*h*4, output appended
static void compressLevel(const u8 *rgba,int w,int h,bool dxt5,std::vector<u8> &out)
{
	int bw=(w+3)/4, bh=(h+3)/4;
	for(int by=0;by<bh;by++) for(int bx=0;bx<bw;bx++){
		u8 px[16][4];
		for(int y=0;y<4;y++) for(int x=0;x<4;x++){
			int sx=bx*4+x, sy=by*4+y;
			if(sx>=w)sx=w-1; if(sy>=h)sy=h-1;
			memcpy(px[y*4+x],rgba+(sy*w+sx)*4,4);
		}
		u8 block[8];
		if(dxt5){ encodeAlphaBlock(px,block); out.insert(out.end(),block,block+8); }
		encodeColorBlock(px,block,!dxt5);
		out.insert(out.end(),block,block+8);
	}
}

static void downsample(const std::vector<u8> &src,int w,int h,std::vector<u8> &dst,int &nw,int &nh)
{
	nw=w>1?w/2:1; nh=h>1?h/2:1;
	dst.resize((size_t)nw*nh*4);
	for(int y=0;y<nh;y++) for(int x=0;x<nw;x++){
		int sx=x*2, sy=y*2;
		int sx1=sx+1<w?sx+1:sx, sy1=sy+1<h?sy+1:sy;
		for(int c=0;c<4;c++){
			int v=src[(sy*w+sx)*4+c]+src[(sy*w+sx1)*4+c]+
			      src[(sy1*w+sx)*4+c]+src[(sy1*w+sx1)*4+c];
			dst[(y*(size_t)nw+x)*4+c]=(u8)(v/4);
		}
	}
}

// ---- TXD rewriting ----

struct Stats { int converted=0, kept=0; size_t saved=0; };

// Maximum texture dimension. Levels above it are dropped: for a mipped DXT
// texture the smaller levels are already in the file, so halving resolution is
// free — no decode, no re-encode. 0 disables the cap.
static int gDimensionCap = 0;

// Drops leading mip levels of an already-compressed texture until it fits the
// cap. Returns empty if nothing to do.
static std::vector<u8> capStruct(const u8 *d, u32 size, const char *txdName, Stats &st)
{
	if(gDimensionCap<=0 || size<88) return {};
	u32 platform=rd32(d);
	if(platform!=8) return {};
	u16 w=rd16(d+80), h=rd16(d+82);
	u8 levels=d[85], comp=d[87];
	if(comp==0 || levels<=1) return {};
	int maxDim = w>h?w:h;
	if(maxDim<=gDimensionCap) return {};
	char name[33]={0}; memcpy(name,d+8,32);

	// count how many leading levels to drop
	int drop=0; int nw=w, nh=h;
	while((nw>gDimensionCap||nh>gDimensionCap) && drop<levels-1){
		nw=nw>1?nw/2:1; nh=nh>1?nh/2:1; drop++;
	}
	if(drop==0) return {};

	// walk the size-prefixed level blobs
	const u8 *p=d+88;
	const u8 *end=d+size;
	size_t dropped=0;
	for(int i=0;i<drop;i++){
		if(p+4>end) return {};
		u32 lv=rd32(p);
		if(p+4+lv>end) return {};
		dropped+=4+lv;
		p+=4+lv;
	}

	std::vector<u8> out;
	out.reserve(size-dropped);
	out.insert(out.end(),d,d+80);                 // header up to width
	wr16(out,(u16)nw); wr16(out,(u16)nh);
	out.push_back(d[84]);                         // depth
	out.push_back((u8)(levels-drop));
	out.push_back(d[86]);                         // type
	out.push_back(comp);
	out.insert(out.end(),p,end);
	st.converted++;
	st.saved+=dropped;
	printf("  %-14s %-24s %4ux%-4u -> %dx%d (%d mips dropped, %.1f MB)\n",
		txdName,name,w,h,nw,nh,drop,dropped/1048576.0);
	return out;
}

// Rebuilds one Texture Native STRUCT payload. Returns empty if left unchanged.
static std::vector<u8> convertStruct(const u8 *d, u32 size, const char *txdName, Stats &st)
{
	if(size<88) return {};
	u32 platform=rd32(d);
	if(platform!=8) return {};
	u32 rasterFormat=rd32(d+72);
	u16 w=rd16(d+80), h=rd16(d+82);
	u8 depth=d[84], levels=d[85], type=d[86], comp=d[87];
	char name[33]={0}; memcpy(name,d+8,32);
	if(comp!=0) return {};                       // already DXT
	if(rasterFormat&0x6000) return {};           // palettized
	if(depth!=32) return {};                     // only plain 32-bit handled
	if(w<8||h<8) return {};
	if(88+(size_t)w*h*4+4>size+4){ /* size sanity below */ }

	// level 0 pixels: stored as size-prefixed blob right after the header
	const u8 *p=d+88;
	u32 lvl0=rd32(p); p+=4;
	if(lvl0<(u32)w*h*4) return {};               // unexpected layout, keep as is
	// D3D A8R8G8B8 is B,G,R,A in memory; swizzle to RGBA
	std::vector<u8> rgba((size_t)w*h*4);
	for(size_t i=0;i<(size_t)w*h;i++){
		rgba[i*4+0]=p[i*4+2]; rgba[i*4+1]=p[i*4+1];
		rgba[i*4+2]=p[i*4+0]; rgba[i*4+3]=p[i*4+3];
	}
	bool hasAlpha=false;
	for(size_t i=0;i<(size_t)w*h;i++) if(rgba[i*4+3]!=255){ hasAlpha=true; break; }
	bool dxt5=hasAlpha;

	// apply the dimension cap before compressing
	while(gDimensionCap>0 && (w>gDimensionCap||h>gDimensionCap)){
		std::vector<u8> half; int nw,nh;
		downsample(rgba,w,h,half,nw,nh);
		rgba.swap(half); w=(u16)nw; h=(u16)nh;
	}

	// mip count for the full chain
	int numLevels=1; { int mw=w,mh=h; while(mw>1||mh>1){ mw=mw>1?mw/2:1; mh=mh>1?mh/2:1; numLevels++; } }

	std::vector<u8> out;
	out.reserve(size);
	wr32(out,8);                                  // platform
	out.insert(out.end(),d+4,d+8);                // filterAddressing
	out.insert(out.end(),d+8,d+72);               // name + mask
	u32 newFormat=(dxt5?0x0500u:0x0200u)|0x8000u; // C8888/C565 + MIPMAP
	wr32(out,newFormat);
	wr32(out,dxt5?1u:0u);                         // hasAlpha
	wr16(out,w); wr16(out,h);
	out.push_back(16);                            // depth
	out.push_back((u8)numLevels);
	out.push_back(type);
	out.push_back(dxt5?5:1);                      // compression

	std::vector<u8> cur=rgba, next;
	int cw=w,ch=h;
	size_t newPixelBytes=0;
	for(int lv=0;lv<numLevels;lv++){
		std::vector<u8> block;
		compressLevel(cur.data(),cw,ch,dxt5,block);
		wr32(out,(u32)block.size());
		out.insert(out.end(),block.begin(),block.end());
		newPixelBytes+=block.size();
		if(lv+1<numLevels){
			int nw,nh; downsample(cur,cw,ch,next,nw,nh);
			cur.swap(next); cw=nw; ch=nh;
		}
	}
	st.converted++;
	st.saved+=(size_t)w*h*4>newPixelBytes ? (size_t)w*h*4-newPixelBytes : 0;
	printf("  %-14s %-24s %4ux%-4u -> %s %d mips (%.1f -> %.1f MB)\n",
		txdName,name,w,h,dxt5?"DXT5":"DXT1",numLevels,
		w*h*4/1048576.0,newPixelBytes/1048576.0);
	return out;
}

// Copies a chunk stream, rebuilding texture natives; returns rebuilt bytes.
static std::vector<u8> rebuildChildren(const u8 *d, u32 len, const char *txdName, Stats &st)
{
	std::vector<u8> out;
	u32 p=0;
	while(p+12<=len){
		u32 type=rd32(d+p), size=rd32(d+p+4), version=rd32(d+p+8);
		if(p+12+size>len) { out.insert(out.end(),d+p,d+len); break; }
		if(type==0x15){
			// texture native: struct + extension children
			const u8 *c=d+p+12; u32 clen=size;
			std::vector<u8> inner;
			u32 q=0;
			while(q+12<=clen){
				u32 ct=rd32(c+q), cs=rd32(c+q+4), cv=rd32(c+q+8);
				if(q+12+cs>clen){ inner.insert(inner.end(),c+q,c+clen); break; }
				if(ct==0x01){
					std::vector<u8> ns=convertStruct(c+q+12,cs,txdName,st);
					if(ns.empty())
						ns=capStruct(c+q+12,cs,txdName,st);
					if(ns.empty()){
						inner.insert(inner.end(),c+q,c+q+12+cs);
						st.kept++;
					}else{
						wr32(inner,0x01); wr32(inner,(u32)ns.size()); wr32(inner,cv);
						inner.insert(inner.end(),ns.begin(),ns.end());
					}
				}else
					inner.insert(inner.end(),c+q,c+q+12+cs);
				q+=12+cs;
			}
			wr32(out,0x15); wr32(out,(u32)inner.size()); wr32(out,version);
			out.insert(out.end(),inner.begin(),inner.end());
		}else{
			out.insert(out.end(),d+p,d+p+12+size);
		}
		p+=12+size;
	}
	return out;
}

static bool processTxd(const char *path)
{
	FILE *f=fopen(path,"rb");
	if(!f){ printf("cannot open %s\n",path); return false; }
	fseek(f,0,SEEK_END); long fileSize=ftell(f); fseek(f,0,SEEK_SET);
	std::vector<u8> data(fileSize);
	fread(data.data(),1,fileSize,f); fclose(f);
	if(fileSize<12 || rd32(data.data())!=0x16){ printf("not a txd: %s\n",path); return false; }
	u32 dictSize=rd32(data.data()+4), dictVersion=rd32(data.data()+8);
	if(12+(long)dictSize>fileSize) dictSize=(u32)(fileSize-12);

	const char *base=strrchr(path,'\\'); base=base?base+1:path;
	Stats st;
	std::vector<u8> body=rebuildChildren(data.data()+12,dictSize,base,st);
	if(st.converted==0){ return true; }

	std::vector<u8> out;
	wr32(out,0x16); wr32(out,(u32)body.size()); wr32(out,dictVersion);
	out.insert(out.end(),body.begin(),body.end());

	f=fopen(path,"wb");
	if(!f){ printf("cannot write %s\n",path); return false; }
	fwrite(out.data(),1,out.size(),f); fclose(f);
	printf("%s: %d converted, %d untouched, %.1f MB pixel data saved, file %.1f -> %.1f MB\n",
		base,st.converted,st.kept,st.saved/1048576.0,
		fileSize/1048576.0,out.size()/1048576.0);
	return true;
}

int main(int argc,char **argv)
{
	if(argc<2){ printf("usage: txdcompress [--cap N] <file.txd> ...\n"); return 1; }
	int failures=0;
	int first=1;
	if(strcmp(argv[1],"--cap")==0 && argc>3){
		gDimensionCap=atoi(argv[2]);
		first=3;
	}
	for(int i=first;i<argc;i++) if(!processTxd(argv[i])) failures++;
	return failures?1:0;
}
