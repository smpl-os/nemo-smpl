/* Original procedural flight scenes. SPDX-License-Identifier: GPL-2.0-or-later */
#include "nemo-flight-renderer.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stdint.h>

#define WORLD_PERIOD 5898240.0
#define GRID_X 320
#define GRID_Z 256
#define TREE_SIDE 96

struct _NemoFlightRenderer {
	GLuint sky, ground, trees, space, stars;
	GLuint vao, grid, indices, instances;
	GLsizei index_count[3];
	gsize index_offset[3];
	float *tree_data;
	GLsizei tree_count;
	int tree_x, tree_z;
	double tree_origin;
	guint32 tree_seed;
	gboolean trees_cached;
};

/* The integer lattice is periodic, unlike sine hashes whose precision changes
 * with distance. CPU clearance and GPU vertices use the same height field. */
static const char noise_glsl[] =
"uint hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}\n"
"uniform uint seed;uniform vec2 worldoffset;\n"
"float h2(ivec2 p){uvec2 q=uvec2(p)&uvec2(4095);return float(hash(q.x+q.y*4099u+seed))/4294967295.;}\n"
"float value_noise(vec2 p){ivec2 i=ivec2(floor(p));vec2 f=fract(p);f=f*f*(3.-2.*f);"
"return mix(mix(h2(i),h2(i+ivec2(1,0)),f.x),mix(h2(i+ivec2(0,1)),h2(i+ivec2(1,1)),f.x),f.y);}\n"
"float land(vec2 p){p=(p+worldoffset)/180.;float broad=value_noise(p*.125);"
"vec2 warp=vec2(value_noise(p*.125+30.),value_noise(p*.125+80.))*2.;"
"float ridge=2.*value_noise(p*.25+warp+19.)-1.;ridge=1.-ridge*ridge;"
"float hills=value_noise(p)*.54+value_noise(p*2.)*.27+value_noise(p*4.)*.13+value_noise(p*8.)*.06;"
"return -78.+broad*155.+pow(ridge,3.)*smoothstep(.40,.74,broad)*520.+hills*78.;}\n";

static const char camera_glsl[] =
"uniform vec3 camera, rightv, upv, forwardv;uniform vec2 resolution;uniform float clock;\n"
"vec4 project(vec3 p){vec3 d=p-camera;float z=dot(d,forwardv);"
"return vec4(dot(d,rightv)*1.48*resolution.y/resolution.x,dot(d,upv)*1.48,"
"1.0002*z-2.0002,z);}\n";

static const char full_vertex[] =
"#version 330 core\nout vec2 uv;\n"
"void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p*2.-1.;gl_Position=vec4(uv,0,1);}\n";

static const char atmosphere_glsl[] =
"vec3 sun=normalize(vec3(-.6,.38,.55));\n"
"vec3 skycolor(vec3 d){float h=max(d.y,0.);"
"vec3 c=mix(vec3(.77,.83,.80),vec3(.17,.39,.64),pow(h,.42));"
"float s=max(dot(d,sun),0.);c+=vec3(1.,.72,.38)*pow(s,96.)*.5;"
"c+=vec3(1.,.87,.63)*smoothstep(.99965,.99987,s)*2.;return c;}\n"
"vec3 fogged(vec3 c,vec3 p){vec3 d=p-camera;float f=1.-exp(-length(d)*.00022);"
"return mix(c,skycolor(normalize(d)),f);}\n"
"vec3 finish(vec3 c){return pow(max(c,vec3(0.)),vec3(.88));}\n";

static const char sky_fragment[] =
"in vec2 uv;out vec4 color;\n"
"void main(){vec3 d=normalize(forwardv+rightv*uv.x*resolution.x/resolution.y/1.48+upv*uv.y/1.48);"
"vec3 c=skycolor(d);"
/* Broad, wind-sheared cloud banks, projected onto a high physical layer. */
"if(d.y>.015){vec2 p=camera.xz+worldoffset+d.xz*(2400.-camera.y)/d.y;"
"float n=value_noise(p/1500.+vec2(sin(clock*.001)*32.,0.))*.65+value_noise(p/630.)*.25+value_noise(p/240.)*.1;"
"float a=smoothstep(.57,.76,n)*smoothstep(.015,.13,d.y)*.65;"
"c=mix(c,vec3(.97,.95,.89),a);}color=vec4(finish(c),1.);}\n";

static const char ground_vertex[] =
"layout(location=0) in vec2 grid;out vec3 position;out vec3 smoothnormal;out float groundheight;out float shadow;\n"
"void main(){float d=-85.+grid.y*grid.y*11800.;"
"vec2 f=normalize(forwardv.xz),r=vec2(f.y,-f.x);"
"vec2 p=camera.xz+f*d+r*grid.x*(140.+max(d,0.)*1.72);"
"float e=12.+max(d,0.)*.006;"
"float l=land(p-vec2(e,0)),rheight=land(p+vec2(e,0)),"
"b=land(p-vec2(0,e)),fheight=land(p+vec2(0,e));"
"groundheight=(land(p)*4.+l+rheight+b+fheight)/8.;"
"position=vec3(p.x,max(18.,groundheight),p.y);"
"smoothnormal=normalize(vec3(l-rheight,2.*e,b-fheight));"
"shadow=1.;for(int i=0;i<3;i++){float rise=i==0?35.:(i==1?100.:240.);"
"float obstruction=land(p+vec2(-.6,.55)/.38*rise)-position.y-rise;"
"shadow=min(shadow,1.-.56*smoothstep(-12.,28.,obstruction));}"
"gl_Position=project(position);}\n";

static const char ground_fragment[] =
"in vec3 position;in vec3 smoothnormal;in float groundheight;in float shadow;out vec4 color;\n"
"void main(){vec3 p=position;vec3 n=normalize(smoothnormal);"
"vec2 soil=mat2(.8,-.6,.6,.8)*(p.xz+worldoffset);"
"float grit=value_noise(soil*.035)*.50+value_noise(soil*.13)*.30+value_noise(soil*.43)*.20;"
"float wet=smoothstep(18.,31.,p.y);"
"float dry=value_noise((p.xz+worldoffset)/1440.+72.);"
"vec3 grass=mix(vec3(.14,.23,.095),vec3(.34,.39,.16),grit);"
"vec3 sand=mix(vec3(.64,.48,.28),vec3(.83,.69,.43),grit);"
"vec3 base=mix(grass,sand,smoothstep(.48,.72,dry));"
"float groves=smoothstep(.29,.50,value_noise((p.xz+worldoffset)/360.+131.));"
"base*=1.-groves*.20*(1.-smoothstep(.48,.64,dry));"
"base=mix(sand,base,wet);"
"float rock=smoothstep(.18,.48,1.-n.y);base=mix(base,vec3(.39,.36,.30)*( .8+grit*.35),rock);"
"base=mix(base,vec3(.82,.85,.82),smoothstep(455.,570.,p.y+n.y*35.));"
"float light=.28+.86*max(dot(n,sun),0.)*shadow;vec3 c=base*light;"
/* Water is the same continuous height field's level set: no lake mesh seams. */
"if(groundheight<18.){vec3 v=normalize(camera-p);"
"vec2 q=(p.xz+worldoffset)*.12;"
"float w1=dot(q,vec2(.8,.6))+clock*.7;"
"float w2=dot(q,vec2(-.35,.94))*1.73-clock;"
"float w3=dot(q,vec2(.95,-.31))*2.41+clock*.6;"
"vec2 ripple=.008*cos(w1)*vec2(.8,.6)+.006*cos(w2)*vec2(-.35,.94)+.004*cos(w3)*vec2(.95,-.31);"
"ripple*=1.-smoothstep(.35,2.5,length(fwidth(q)));vec3 wn=normalize(vec3(ripple.x,1.,ripple.y));"
"float fres=.05+.85*pow(1.-max(dot(v,wn),0.),4.);"
"vec3 reflected=skycolor(reflect(-v,wn));"
"vec3 water=mix(vec3(.035,.18,.19),vec3(.12,.34,.30),exp((groundheight-18.)*.07));"
"c=mix(water,reflected,fres)+vec3(1.,.85,.57)*pow(max(dot(reflect(-sun,wn),v),0.),170.)*.9;"
"c=mix(c,vec3(.52,.65,.56),.25*(1.-smoothstep(0.,2.2,18.-groundheight)));}"
"color=vec4(finish(fogged(c,p)),1.);}\n";

static const char tree_vertex[] =
"layout(location=1) in vec4 instance;layout(location=2) in vec2 variation;"
"out vec3 position;out vec3 normal;out float tint;uniform int geometrylod;\n"
"void main(){vec2 delta=instance.xz-camera.xz;float dist=length(delta);"
"float ahead=dot(delta,normalize(forwardv.xz));"
"float sideways=abs(dot(delta,normalize(rightv.xz)));"
"position=instance.xyz;normal=vec3(0.,1.,0.);tint=0.;gl_Position=vec4(2.,2.,2.,1.);"
"if(dist>1400.||ahead< -50.||sideways>max(ahead,0.)*1.55+100.){"
"return;}"
"float a=variation.x,b=mod(variation.y,2.);bool leafy=variation.y>=2.;"
"float fade=1.-smoothstep(1150.,1400.,dist);"
"float height=instance.w*fade;int v=gl_VertexID;vec3 local=vec3(0.);int trunk;"
"if(leafy){int segments=geometrylod>0?8:10,bands=geometrylod>0?3:4;"
"int crown=segments*bands*6;trunk=v-crown;"
"if(v<crown){int tile=v/6,corner=v%6;int band=tile/segments,sector=tile%segments;"
"int sx=(corner==1||corner==2||corner==4)?1:0;"
"int sy=(corner==2||corner==3||corner==4)?1:0;"
"float lon=float(sector+sx)*6.2831853/float(segments)+a*6.2831853;"
"float lat=float(band+sy)*3.14159265/float(bands);"
"float irregular=1.+.13*sin(lon*3.+a*30.)+.09*cos(lon*5.+lat*3.+b*20.);"
"vec3 n=vec3(cos(lon)*sin(lat),cos(lat),sin(lon)*sin(lat));"
"local=vec3(n.x*(7.+a*5.)*irregular,(.60+n.y*.40)*height,n.z*(6.+b*5.)*irregular);"
"normal=normalize(n*vec3(1.,.75,1.)+vec3(.12*sin(lon*5.+lat*4.),.07*cos(lon*3.),.10*cos(lon*7.-lat*3.)));}}"
"else{int crowns=geometrylod>0?48:72;trunk=v-crowns;"
"if(v<crowns){int layer=v/24,tri=(v%24)/3,corner=v%3;"
"float angle=float(tri+(corner==2?1:0))*.785398+a*6.2831853;"
"float branch=.82+.30*sin(angle*3.+float(layer)*11.+a*38.);"
"float radius=(corner==0?0.:(5.+a*3.)*(1.-float(layer)*.22)*branch);"
"float y=corner==0?(.70+float(layer)*(geometrylod>0?.30:.15)):(.12+float(layer)*.18);"
"local=vec3(cos(angle)*radius,y*height,sin(angle)*radius);"
"normal=normalize(vec3(cos(angle+.39),.55,sin(angle+.39)));}}"
"if(trunk>=12)return;"
"if(trunk>=0){int j=trunk;float s=(j%6==0||j%6==3||j%6==5)?-1.:1.;"
"float yy=(j%6>=2&&j%6<=4)?.36:-.12;"
"local=vec3(j<6?s*.65:0.,yy*height,j>=6?s*.65:0.);"
"normal=vec3(j<6?0.:1.,0.,j<6?1.:0.);}"
"position=instance.xyz+local;tint=trunk>=0?-1.:(leafy?b+2.:b);"
"gl_Position=project(position);if(height<=0.)gl_Position=vec4(2.,2.,2.,1.);}\n";

static const char tree_fragment[] =
"in vec3 position;in vec3 normal;in float tint;out vec4 color;\n"
"void main(){float v=mod(max(tint,0.),2.);vec3 c=mix(vec3(.035,.105,.065),vec3(.15,.23,.09),v);"
"if(tint>=2.)c=mix(vec3(.10,.19,.055),vec3(.29,.34,.10),v);"
"if(tint<0.)c=vec3(.18,.12,.065);"
"else{vec2 leaf=(position.xz+worldoffset)*1.1+position.y*vec2(.31,.47);"
"float grain=value_noise(leaf)*.65+value_noise(leaf*2.7)*.35;"
"float rim=1.-abs(dot(normalize(normal),normalize(camera-position)));"
"if(grain<max(0.,rim-.60)*(tint>=2.?.72:.35))discard;"
"c*=.68+grain*.58;c*=.76+.24*max(normal.y,0.);}"
"c*=.48+.85*max(dot(normalize(normal),sun),0.);"
"color=vec4(finish(fogged(c,position)),1.);}\n";

static const char space_fragment[] =
"in vec2 uv;out vec4 color;uniform float travel;uniform int sector;uniform float energy;\n"
"float h3(vec3 p){ivec3 q=ivec3(floor(p));vec3 f=fract(p);f=f*f*(3.-2.*f);"
"float a=h2(q.xy+q.z*ivec2(37,71));float b=h2(q.xy+ivec2(1,0)+q.z*ivec2(37,71));"
"float c=h2(q.xy+ivec2(0,1)+q.z*ivec2(37,71));float d=h2(q.xy+1+q.z*ivec2(37,71));"
"float e=h2(q.xy+(q.z+1)*ivec2(37,71));float f1=h2(q.xy+ivec2(1,0)+(q.z+1)*ivec2(37,71));"
"float g=h2(q.xy+ivec2(0,1)+(q.z+1)*ivec2(37,71));float h=h2(q.xy+1+(q.z+1)*ivec2(37,71));"
"return mix(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),mix(mix(e,f1,f.x),mix(g,h,f.x),f.y),f.z);}\n"
"float fbm(vec3 p){return h3(p)*.52+h3(p*2.03)*.26+h3(p*4.07)*.14+h3(p*8.13)*.08;}\n"
"void main(){vec3 rd=normalize(vec3(uv.x*resolution.x/resolution.y,uv.y,1.48));"
"vec3 ld=normalize(vec3(-.65,.45,-.60));"
"float mist=fbm(rd*5.+vec3(13.,float(seed%1000u)*.01,2.));"
"float band=pow(max(0.,1.-abs(rd.y*.8-rd.x*.32+.12)),12.);"
"vec3 c=vec3(.0015,.0025,.008)+band*vec3(.025,.034,.07)*mist;"
"vec2 st=rd.xy/ max(rd.z,.01)*700.;ivec2 cell=ivec2(floor(st));"
"float star=h2(cell+ivec2(19,81));"
"float point=exp(-dot(fract(st)-.5,fract(st)-.5)*230.);"
"c+=vec3(.54,.65,1.)*point*step(.987,star)*(.2+star);"
"float nearest=100000.;vec3 hit=vec3(0.),center=vec3(0.);float radius=1.,kind=0.,key=0.;"
"for(int j=-1;j<5;j++){int k=sector+j;float a=h2(ivec2(k,317)),b=h2(ivec2(k,811));"
"float r=1050.+a*1150.;"
"vec3 pc=vec3((a>.5?1.:-1.)*(r+550.+b*1600.),(b-.5)*3100.,float(j)*19000.+7000.-travel);"
"float along=dot(rd,pc),disc=along*along-dot(pc,pc)+r*r;"
"if(disc>0.){float t=along-sqrt(disc);if(t>1.&&t<nearest){nearest=t;hit=rd*t;center=pc;radius=r;kind=mod(float(k)+float(seed%31u),4.);key=a;}}"
/* The atmosphere is an analytic shell, not a painted planet outline. */
"float closest=max(along,0.);float limb=length(rd*closest-pc)/r;"
"float glow=exp(-max(limb-1.,0.)*45.)*step(1.,limb)*step(0.,along);"
"c+=mix(vec3(.055,.15,.29),vec3(.32,.17,.055),step(.5,b))*glow*.28;}"
"gl_FragDepth=1.;"
"if(nearest<100000.){vec3 n=normalize(hit-center);vec3 p=n*3.+vec3(key*31.,key*17.,7.);"
"float v=fbm(p),detail=fbm(p*5.);vec3 base;"
"if(kind<.5){float continents=smoothstep(.47,.54,v);"
"vec3 earth=mix(vec3(.016,.09,.19),mix(vec3(.11,.24,.085),vec3(.45,.38,.20),detail),continents);"
"earth=mix(earth,vec3(.80,.86,.88),smoothstep(.79,.94,abs(n.y)+v*.10));"
"float cloud=smoothstep(.57,.70,fbm(p*2.+vec3(sin(clock*.001)*4.,0.,0.)));base=mix(earth,vec3(.86,.87,.82),cloud*.88);}"
"else if(kind<1.5){float bands=.5+.5*sin(n.y*37.+h3(p*2.)*5.);"
"base=mix(vec3(.24,.13,.09),vec3(.80,.62,.37),bands*.65+v*.35);"
"float storm=exp(-length((n.xy-vec2(.23,-.18))*vec2(16.,33.)));base=mix(base,vec3(.61,.24,.12),storm);}"
"else if(kind<2.5){base=mix(vec3(.23,.075,.035),vec3(.72,.40,.19),v);base*=.75+detail*.5;}"
"else{base=mix(vec3(.16,.21,.24),vec3(.54,.64,.65),v);base*=.70+detail*.5;}"
"float diffuse=max(dot(n,ld),0.);float rim=pow(1.-max(dot(n,-rd),0.),3.);"
"c=base*(.035+1.35*diffuse)+vec3(.13,.29,.45)*rim*smoothstep(-.2,.3,dot(n,ld));"
"if(kind<.5)c+=vec3(.7,.82,.9)*pow(max(dot(reflect(-ld,n),-rd),0.),75.)*.32*step(v,.50);"
"float z=hit.z;gl_FragDepth=.5*(1.0002-2.0002/z)+.5;}"
"c*=1.+energy*.08;float vignette=1.-.12*dot(uv*.5,uv*.5);"
"color=vec4(pow(max(c*vignette,vec3(0.)),vec3(.82)),1.);}\n";

static const char star_vertex[] =
"out vec2 shape;out vec3 tint;out float visibility;uniform float startravel;uniform int starsector;\n"
"void main(){int id=gl_InstanceID;float c=h2(ivec2(id,7));"
"int cell=starsector+(c*24000.<startravel?1:0);"
"float a=h2(ivec2(id+cell*67,3)),b=h2(ivec2(id+cell*67,5));"
"float z=mod(c*24000.-startravel,24000.)+20.;"
"visibility=1.-smoothstep(22000.,24020.,z);"
"vec3 p=vec3((a-.5)*13000.,(b-.5)*8000.,z);"
"vec2 screen=p.xy*1.48/z;screen.x*=resolution.y/resolution.x;"
"vec2 dir=normalize(screen+vec2(.0001));"
"vec2 side=vec2(-dir.y,dir.x);"
"int v=gl_VertexID;vec2 q=vec2((v==1||v==2||v==4)?1.:-1.,(v>=2&&v<=4)?1.:-1.);"
"float len=clamp(17000./z,.7,27.);"
"vec2 offset=(side*q.x*.65+dir*q.y*len)*2./resolution;"
"gl_Position=vec4((screen+offset)*z,1.0002*z-2.0002,z);"
"shape=q;tint=mix(vec3(.58,.73,1.),vec3(1.,.86,.64),h2(ivec2(id,11)))*(.55+.6*a);}\n";

static const char star_fragment[] =
"in vec2 shape;in vec3 tint;in float visibility;out vec4 color;\n"
"void main(){float a=exp(-shape.x*shape.x*4.)*(1.-smoothstep(.45,1.,abs(shape.y)));"
"color=vec4(tint*a*visibility,a*visibility);}\n";

static uint32_t
hash_u32 (uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15;
	x *= 0x846ca68bU; return x ^ (x >> 16);
}

static double
lattice (int x, int z, guint32 seed)
{
	return hash_u32 (((uint32_t) x & 4095U) +
	                 ((uint32_t) z & 4095U) * 4099U + seed) / 4294967295.0;
}

static double
noise_cpu (double x, double z, guint32 seed)
{
	int ix = floor (x), iz = floor (z);
	double a = x - ix, b = z - iz;
	a = a * a * (3 - 2 * a); b = b * b * (3 - 2 * b);
	double u = lattice (ix, iz, seed) * (1 - a) + lattice (ix + 1, iz, seed) * a;
	double v = lattice (ix, iz + 1, seed) * (1 - a) + lattice (ix + 1, iz + 1, seed) * a;
	return u * (1 - b) + v * b;
}

static double
height_cpu (double x, double z, guint32 seed)
{
	x /= 180; z /= 180;
	double broad = noise_cpu (x * .125, z * .125, seed);
	double wx = noise_cpu (x * .125 + 30, z * .125 + 30, seed) * 2;
	double wz = noise_cpu (x * .125 + 80, z * .125 + 80, seed) * 2;
	double ridge = 2 * noise_cpu (x * .25 + wx + 19, z * .25 + wz + 19, seed) - 1;
	ridge = 1 - ridge * ridge;
	double t = CLAMP ((broad - .40) / .34, 0, 1);
	double hills = noise_cpu (x, z, seed) * .54 + noise_cpu (x * 2, z * 2, seed) * .27 +
	               noise_cpu (x * 4, z * 4, seed) * .13 + noise_cpu (x * 8, z * 8, seed) * .06;
	return -78 + broad * 155 + pow (ridge, 3) * t * t * (3 - 2 * t) * 520 + hills * 78;
}

static double
filtered_height (double x, double z, guint32 seed)
{
	return (height_cpu (x, z, seed) * 4 +
	        height_cpu (x - 12, z, seed) + height_cpu (x + 12, z, seed) +
	        height_cpu (x, z - 12, seed) + height_cpu (x, z + 12, seed)) / 8;
}

static void
update_trees (NemoFlightRenderer *self, const float camera[3], double origin, guint32 seed)
{
	int cx = floor (camera[0] / 30), cz = floor ((camera[2] + origin) / 30);
	if (self->trees_cached && self->tree_x == cx && self->tree_z == cz &&
	    self->tree_seed == seed && self->tree_origin == origin)
		return;
	self->tree_count = 0;
	for (int z = cz - 24; z < cz + 72; z++)
		for (int x = cx - 48; x < cx + 48; x++) {
			double a = lattice (x, z, seed), b = lattice (x + 71, z + 19, seed);
			double px = (x + .08 + .84 * a) * 30, pz = (z + .08 + .84 * b) * 30;
			double dry = noise_cpu (px / 1440 + 72, pz / 1440 + 72, seed);
			double forest = noise_cpu (px / 360 + 131, pz / 360 + 131, seed);
			if (dry > .60 || lattice (x + 183, z + 257, seed) >
			    CLAMP ((forest - .27) * 2.8, 0, .98))
				continue;
			double h = filtered_height (px, pz, seed);
			if (h < 30 || h > 365) continue;
			float *v = self->tree_data + self->tree_count++ * 6;
			v[0] = px; v[1] = h; v[2] = pz - origin;
			v[3] = 13 + a * 19;
			v[4] = a; v[5] = b + (h < 180 && b > .36 ? 2 : 0);
		}
	glBindBuffer (GL_ARRAY_BUFFER, self->instances);
	glBufferSubData (GL_ARRAY_BUFFER, 0, self->tree_count * 6 * sizeof (float), self->tree_data);
	self->tree_x = cx; self->tree_z = cz; self->tree_seed = seed;
	self->tree_origin = origin; self->trees_cached = TRUE;
}

static double
route_x (double z, guint32 seed)
{
	double phase = (seed % 4096) * (G_PI / 2048);
	return 530 * sin (z * (2 * G_PI * 184 / WORLD_PERIOD) + phase) +
	       240 * sin (z * (2 * G_PI * 408 / WORLD_PERIOD) + phase * .7);
}

static double
altitude (double z, guint32 seed)
{
	/* A smooth weighted envelope sees hills before the aircraft reaches them.
	 * The log-sum-exp envelope avoids hard max slope changes at sample handoffs. */
	double sum = 0;
	for (int i = -4; i <= 8; i++) {
		double dz = i * 95;
		double h = MAX (18, height_cpu (route_x (z + dz, seed), z + dz, seed));
		h -= dz * dz * .00024;
		sum += exp (h / 45);
	}
	return log (sum) * 45 + 68;
}

static void
normalize3 (float v[3])
{
	float n = sqrtf (v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
	for (int i = 0; i < 3; i++) v[i] /= n;
}

static GLuint
compile_shader (GLenum type, const char *body, gboolean common,
                gboolean atmosphere, GError **error)
{
	GLuint shader = glCreateShader (type);
	const char *parts[] = { "#version 330 core\n", noise_glsl, camera_glsl,
	                       atmosphere ? atmosphere_glsl : "", body };
	if (common) glShaderSource (shader, G_N_ELEMENTS (parts), parts, NULL);
	else glShaderSource (shader, 1, &body, NULL);
	glCompileShader (shader);
	GLint ok;
	glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096] = { 0 };
		glGetShaderInfoLog (shader, sizeof log, NULL, log);
		g_set_error (error, g_quark_from_static_string ("nemo-flight-renderer"), 1,
		             "Flight shader compilation failed: %s", log);
		glDeleteShader (shader);
		return 0;
	}
	return shader;
}

static GLuint
make_program (const char *vertex, const char *fragment, gboolean fullscreen,
              gboolean atmosphere, GError **error)
{
	GLuint vs = compile_shader (GL_VERTEX_SHADER, vertex, !fullscreen, FALSE, error);
	if (!vs) return 0;
	GLuint fs = compile_shader (GL_FRAGMENT_SHADER, fragment, TRUE, atmosphere, error);
	if (!fs) { glDeleteShader (vs); return 0; }
	GLuint program = glCreateProgram ();
	glAttachShader (program, vs); glAttachShader (program, fs);
	glLinkProgram (program);
	glDeleteShader (vs); glDeleteShader (fs);
	GLint ok;
	glGetProgramiv (program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096] = { 0 };
		glGetProgramInfoLog (program, sizeof log, NULL, log);
		g_set_error (error, g_quark_from_static_string ("nemo-flight-renderer"), 2,
		             "Flight shader linking failed: %s", log);
		glDeleteProgram (program);
		return 0;
	}
	return program;
}

NemoFlightRenderer *
nemo_flight_renderer_new (GError **error)
{
	NemoFlightRenderer *self = g_new0 (NemoFlightRenderer, 1);
	GLint depth_bits = 0;
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, GL_DEPTH,
	                                      GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth_bits);
	if (depth_bits < 16) {
		g_set_error_literal (error, g_quark_from_static_string ("nemo-flight-renderer"), 6,
		                     "Flight scenes require an EGL default framebuffer with a depth buffer");
		goto fail;
	}
	self->sky = make_program (full_vertex, sky_fragment, TRUE, TRUE, error);
	if (!self->sky) goto fail;
	self->ground = make_program (ground_vertex, ground_fragment, FALSE, TRUE, error);
	if (!self->ground) goto fail;
	self->trees = make_program (tree_vertex, tree_fragment, FALSE, TRUE, error);
	if (!self->trees) goto fail;
	self->space = make_program (full_vertex, space_fragment, TRUE, FALSE, error);
	if (!self->space) goto fail;
	self->stars = make_program (star_vertex, star_fragment, FALSE, FALSE, error);
	if (!self->stars) goto fail;

	glGenVertexArrays (1, &self->vao);
	glBindVertexArray (self->vao);
	float *vertices = g_new (float, (GRID_X + 1) * (GRID_Z + 1) * 2);
	GLuint *indices = g_new (GLuint, GRID_X * GRID_Z * 6 * 21 / 16);
	for (int z = 0; z <= GRID_Z; z++)
		for (int x = 0; x <= GRID_X; x++) {
			int i = (z * (GRID_X + 1) + x) * 2;
			vertices[i] = 2.f * x / GRID_X - 1;
			vertices[i + 1] = (float) z / GRID_Z;
		}
	gsize count = 0;
	for (int level = 0; level < 3; level++) {
		int step = 1 << level, stride = (GRID_X + 1) * step;
		self->index_offset[level] = count * sizeof (GLuint);
		for (int z = 0; z < GRID_Z; z += step)
			for (int x = 0; x < GRID_X; x += step) {
				int v = z * (GRID_X + 1) + x;
				indices[count++] = v; indices[count++] = v + step; indices[count++] = v + stride;
				indices[count++] = v + step; indices[count++] = v + stride + step;
				indices[count++] = v + stride;
			}
		self->index_count[level] = GRID_X * GRID_Z * 6 / (step * step);
	}
	glGenBuffers (1, &self->grid);
	glBindBuffer (GL_ARRAY_BUFFER, self->grid);
	glBufferData (GL_ARRAY_BUFFER, (GRID_X+1)*(GRID_Z+1)*2*sizeof(float), vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glGenBuffers (1, &self->indices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, self->indices);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, count * sizeof (GLuint), indices, GL_STATIC_DRAW);
	self->tree_data = g_new (float, TREE_SIDE * TREE_SIDE * 6);
	glGenBuffers (1, &self->instances);
	glBindBuffer (GL_ARRAY_BUFFER, self->instances);
	glBufferData (GL_ARRAY_BUFFER, TREE_SIDE * TREE_SIDE * 6 * sizeof (float), NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof (float), NULL);
	glVertexAttribDivisor (1, 1);
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof (float), (const void *) (4 * sizeof (float)));
	glVertexAttribDivisor (2, 1);
	g_free (vertices); g_free (indices);
	GLenum status = glGetError ();
	if (status != GL_NO_ERROR) {
		g_set_error (error, g_quark_from_static_string ("nemo-flight-renderer"), 3,
		             "Flight GPU initialization failed (OpenGL 0x%x)", status);
		goto fail;
	}
	return self;
fail:
	nemo_flight_renderer_free (self);
	return NULL;
}

static void
uniforms (GLuint program, guint32 seed, double time, float energy,
          int width, int height, const float camera[3], const float right[3],
          const float up[3], const float forward[3])
{
	glUseProgram (program);
	glUniform1ui (glGetUniformLocation (program, "seed"), seed);
	glUniform1i (glGetUniformLocation (program, "geometrylod"), width <= 480 ? 1 : 0);
	glUniform2f (glGetUniformLocation (program, "worldoffset"), 0,
	             floor (fmod (time, WORLD_PERIOD / 95) * 95 / 1024) * 1024);
	glUniform1f (glGetUniformLocation (program, "clock"), fmod (time, 20000 * G_PI));
	glUniform1f (glGetUniformLocation (program, "energy"), energy);
	glUniform2f (glGetUniformLocation (program, "resolution"), width, height);
	glUniform3fv (glGetUniformLocation (program, "camera"), 1, camera);
	glUniform3fv (glGetUniformLocation (program, "rightv"), 1, right);
	glUniform3fv (glGetUniformLocation (program, "upv"), 1, up);
	glUniform3fv (glGetUniformLocation (program, "forwardv"), 1, forward);
	glUniform1f (glGetUniformLocation (program, "travel"), fmod (time, 19000.0 / 1450) * 1450);
	glUniform1f (glGetUniformLocation (program, "startravel"), fmod (time, 24000.0 / 1450) * 1450);
	glUniform1i (glGetUniformLocation (program, "starsector"),
	             (int) floor (fmod (time, 24000.0 * 4096 / 1450) * 1450 / 24000));
	glUniform1i (glGetUniformLocation (program, "sector"),
	             (int) floor (fmod (time, 19000.0 * 4096 / 1450) * 1450 / 19000));
}

gboolean
nemo_flight_renderer_render (NemoFlightRenderer *self, NemoFlightScene scene,
                            guint32 seed, double time, double energy,
                            int width, int height, GError **error)
{
	if (!self || (scene != NEMO_FLIGHT_SPACE && scene != NEMO_FLIGHT_TERRAIN) ||
	    width < 1 || height < 1 || !isfinite (time) || time < 0 || !isfinite (energy)) {
		g_set_error_literal (error, g_quark_from_static_string ("nemo-flight-renderer"), 4,
		                     "Invalid flight renderer frame arguments");
		return FALSE;
	}
	float camera[3] = { 0 }, right[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 }, forward[3] = { 0, 0, 1 };
	energy = CLAMP (energy, 0, 1);
	if (scene == NEMO_FLIGHT_TERRAIN) {
		double z = fmod (time, WORLD_PERIOD / 95) * 95;
		camera[0] = route_x (z, seed);
		camera[1] = altitude (z, seed);
		camera[2] = fmod (z, 1024);
		forward[0] = route_x (z + 550, seed) - camera[0];
		forward[1] = CLAMP (altitude (z + 550, seed) - camera[1] - 33, -75, 65);
		forward[2] = 550;
		normalize3 (forward);
		right[0] = forward[2]; right[1] = 0; right[2] = -forward[0];
		normalize3 (right);
		up[0] = forward[1]*right[2]; up[1] = forward[2]*right[0]-forward[0]*right[2];
		up[2] = -forward[1]*right[0];
		float roll = .035 * sin (z * (2 * G_PI * 184 / WORLD_PERIOD) + seed % 100);
		for (int i = 0; i < 3; i++) {
			float r = right[i], u = up[i];
			right[i] = r*cosf(roll)+u*sinf(roll);
			up[i] = u*cosf(roll)-r*sinf(roll);
		}
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (0, 0, width, height);
	glDisable (GL_SCISSOR_TEST); glDisable (GL_STENCIL_TEST);
	glDisable (GL_CULL_FACE); glDisable (GL_BLEND);
	glDisable (GL_RASTERIZER_DISCARD); glDisable (GL_FRAMEBUFFER_SRGB);
	glDisable (GL_POLYGON_OFFSET_FILL);
	glDisable (GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable (GL_SAMPLE_COVERAGE);
	glDisable (GL_COLOR_LOGIC_OP); glDisable (GL_PRIMITIVE_RESTART);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask (GL_TRUE); glDepthFunc (GL_LEQUAL);
	glDepthRange (0, 1);
	glClearDepth (1); glClearColor (0, 0, 0, 1);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray (self->vao);
	if (scene == NEMO_FLIGHT_TERRAIN) {
		int level = width <= 320 ? 2 : (width <= 480 ? 1 : 0);
		update_trees (self, camera, floor (fmod (time, WORLD_PERIOD / 95) * 95 / 1024) * 1024, seed);
		glDisable (GL_DEPTH_TEST);
		uniforms (self->sky, seed, time, energy, width, height, camera, right, up, forward);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		glEnable (GL_DEPTH_TEST);
		uniforms (self->ground, seed, time, energy, width, height, camera, right, up, forward);
		glDrawElements (GL_TRIANGLES, self->index_count[level], GL_UNSIGNED_INT,
		                (const void *) self->index_offset[level]);
		uniforms (self->trees, seed, time, energy, width, height, camera, right, up, forward);
		glDrawArraysInstanced (GL_TRIANGLES, 0, width <= 480 ? 156 : 252, self->tree_count);
	} else {
		glEnable (GL_DEPTH_TEST);
		uniforms (self->space, seed, time, energy, width, height, camera, right, up, forward);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		uniforms (self->stars, seed, time, energy, width, height, camera, right, up, forward);
		glEnable (GL_BLEND); glBlendFunc (GL_ONE, GL_ONE);
		glDepthMask (GL_FALSE);
		glDrawArraysInstanced (GL_TRIANGLES, 0, 6, 6200);
		glDepthMask (GL_TRUE); glDisable (GL_BLEND);
	}
	glBindVertexArray (0);
	glUseProgram (0);
	GLenum status = glGetError ();
	if (status != GL_NO_ERROR) {
		g_set_error (error, g_quark_from_static_string ("nemo-flight-renderer"), 5,
		             "Flight rendering failed (OpenGL 0x%x)", status);
		return FALSE;
	}
	return TRUE;
}

void
nemo_flight_renderer_free (NemoFlightRenderer *self)
{
	if (!self) return;
	glDeleteProgram (self->sky); glDeleteProgram (self->ground);
	glDeleteProgram (self->trees); glDeleteProgram (self->space);
	glDeleteProgram (self->stars);
	glDeleteBuffers (1, &self->grid); glDeleteBuffers (1, &self->indices);
	glDeleteBuffers (1, &self->instances);
	glDeleteVertexArrays (1, &self->vao);
	g_free (self->tree_data);
	g_free (self);
}
