#include "quakedef.h"
#ifdef VKQUAKE
#include "vkrenderer.h"
#include "glquake.h"
#include "gl_draw.h"
#include "shader.h"

//FIXME: instead of switching rendertargets and back, we should be using an alternative queue.

#define PERMUTATION_BEM_DEPTHONLY (1u<<14)
#define PERMUTATION_BEM_WIREFRAME (1u<<15)

#undef BE_Init
#undef BE_SelectMode
#undef BE_GenBrushModelVBO
#undef BE_ClearVBO
#undef BE_UploadAllLightmaps
#undef BE_LightCullModel
#undef BE_SelectEntity
#undef BE_SelectDLight
#undef BE_GetTempBatch
#undef BE_SubmitBatch
#undef BE_DrawMesh_List
#undef BE_DrawMesh_Single
#undef BE_SubmitMeshes
#undef BE_DrawWorld
#undef BE_VBO_Begin
#undef BE_VBO_Data
#undef BE_VBO_Finish
#undef BE_VBO_Destroy
#undef BE_Scissor

#undef BE_RenderToTextureUpdate2d

extern cvar_t r_shadow_realtime_world_lightmaps;
extern cvar_t gl_overbright;
extern cvar_t r_portalrecursion;

extern cvar_t r_polygonoffset_shadowmap_offset, r_polygonoffset_shadowmap_factor;
extern cvar_t r_wireframe;
extern cvar_t vk_stagingbuffers;

unsigned int vk_usedynamicstaging;

static void VK_TerminateShadowMap(void);
void VKBE_BeginShadowmapFace(void);

static void R_DrawPortal(batch_t *batch, batch_t **blist, batch_t *depthmasklist[2], int portaltype);

#define MAX_TMUS 32

extern texid_t r_whiteimage, missing_texture_gloss, missing_texture_normal;
texid_t r_blackimage;

static void BE_RotateForEntity (const entity_t *e, const model_t *mod);
void VKBE_SetupLightCBuffer(dlight_t *l, vec3_t colour);

/*========================================== tables for deforms =====================================*/
#define frand() (rand()*(1.0/RAND_MAX))
#define FTABLE_SIZE		1024
#define FTABLE_CLAMP(x)	(((int)((x)*FTABLE_SIZE) & (FTABLE_SIZE-1)))
#define FTABLE_EVALUATE(table,x) (table ? table[FTABLE_CLAMP(x)] : frand()*((x)-floor(x)))
#define R_FastSin(x) r_sintable[FTABLE_CLAMP(x)]

static	float	r_sintable[FTABLE_SIZE];
static	float	r_triangletable[FTABLE_SIZE];
static	float	r_squaretable[FTABLE_SIZE];
static	float	r_sawtoothtable[FTABLE_SIZE];
static	float	r_inversesawtoothtable[FTABLE_SIZE];

static float *FTableForFunc ( unsigned int func )
{
	switch (func)
	{
		case SHADER_FUNC_SIN:
			return r_sintable;

		case SHADER_FUNC_TRIANGLE:
			return r_triangletable;

		case SHADER_FUNC_SQUARE:
			return r_squaretable;

		case SHADER_FUNC_SAWTOOTH:
			return r_sawtoothtable;

		case SHADER_FUNC_INVERSESAWTOOTH:
			return r_inversesawtoothtable;
	}

	//bad values allow us to crash (so I can debug em)
	return NULL;
}

static void FTable_Init(void)
{
	unsigned int i;
	double t;
	for (i = 0; i < FTABLE_SIZE; i++)
	{
		t = (double)i / (double)FTABLE_SIZE;

		r_sintable[i] = sin(t * 2*M_PI);

		if (t < 0.25)
			r_triangletable[i] = t * 4.0;
		else if (t < 0.75)
			r_triangletable[i] = 2 - 4.0 * t;
		else
			r_triangletable[i] = (t - 0.75) * 4.0 - 1.0;

		if (t < 0.5)
			r_squaretable[i] = 1.0f;
		else
			r_squaretable[i] = -1.0f;

		r_sawtoothtable[i] = t;
		r_inversesawtoothtable[i] = 1.0 - t;
	}
}

typedef vec3_t mat3_t[3];
static mat3_t axisDefault={{1, 0, 0},
					{0, 1, 0},
					{0, 0, 1}};

static void Matrix3_Transpose (mat3_t in, mat3_t out)
{
	out[0][0] = in[0][0];
	out[1][1] = in[1][1];
	out[2][2] = in[2][2];

	out[0][1] = in[1][0];
	out[0][2] = in[2][0];
	out[1][0] = in[0][1];
	out[1][2] = in[2][1];
	out[2][0] = in[0][2];
	out[2][1] = in[1][2];
}
static void Matrix3_Multiply_Vec3 (const mat3_t a, const vec3_t b, vec3_t product)
{
	product[0] = a[0][0]*b[0] + a[0][1]*b[1] + a[0][2]*b[2];
	product[1] = a[1][0]*b[0] + a[1][1]*b[1] + a[1][2]*b[2];
	product[2] = a[2][0]*b[0] + a[2][1]*b[1] + a[2][2]*b[2];
}

static int Matrix3_Compare(const mat3_t in, const mat3_t out)
{
	return !memcmp(in, out, sizeof(mat3_t));
}

/*================================================*/

//dlight-specific constant-buffer
typedef struct
{
	float l_cubematrix[16];
	vec3_t l_lightposition; float padl1;
	vec3_t l_colour; float pad2;
	vec3_t l_lightcolourscale; float l_lightradius;
	vec4_t l_shadowmapproj;
	vec2_t l_shadowmapscale; vec2_t pad3;
} cbuf_light_t;

//entity-specific constant-buffer
typedef struct
{
	float m_modelviewproj[16];
	float m_model[16];
	float m_modelinv[16];
	vec3_t e_eyepos;
	float e_time;
	vec3_t e_light_ambient; float pad1;
	vec3_t e_light_dir;		float pad2;
	vec3_t e_light_mul;		float pad3;
	vec4_t e_lmscale[4];
	vec3_t e_uppercolour;	float pad4;
	vec3_t e_lowercolour;	float pad5;
	vec3_t e_glowmod;		float pad6;
	vec4_t e_colourident;
	vec4_t w_fogcolours;
	float w_fogdensity;		float w_fogdepthbias;	vec2_t pad7;
} cbuf_entity_t;

enum 
{
	VK_BUFF_POS,
	VK_BUFF_TC,
	VK_BUFF_COL,
	VK_BUFF_LMTC,
	VK_BUFF_NORM,
	VK_BUFF_SDIR,
	VK_BUFF_TDIR,
	VK_BUFF_MAX
};

typedef struct
{
	unsigned int inited;

	backendmode_t mode;
	unsigned int modepermutation;
	unsigned int flags;
	unsigned int forcebeflags;

	float	identitylighting;
	float	identitylightmap;
	float		curtime;
	const entity_t	*curentity;
	const dlight_t	*curdlight;
//	vec3_t		curdlight_colours;
	shader_t	*curshader;
	shader_t	*depthonly;
	texnums_t	*curtexnums;
	int			curvertdecl;
//	unsigned int shaderbits;
//	unsigned int curcull;
//	float depthbias;
//	float depthfactor;
//	unsigned int lastpasscount;
	vbo_t *batchvbo;
	batch_t *curbatch;
	batch_t dummybatch;
	vec4_t lightshadowmapproj;
	vec2_t lightshadowmapscale;

	unsigned int curlmode;
	shader_t	*shader_rtlight[LSHADER_MODES];

	program_t			*programfixedemu[2];

	mesh_t		**meshlist;
	unsigned int nummeshes;

	unsigned int wbatch;
	unsigned int maxwbatches;
	batch_t *wbatches;

	VkDescriptorBufferInfo ubo_entity;
	VkDescriptorBufferInfo ubo_light;
	vec4_t lightinfo;	//org+radius

	VkBuffer staticbuf;	//holds fallback vertex info so we don't crash from it
	VkDeviceMemory staticbufmem;

	texid_t tex_currentrender;

	struct vk_rendertarg rt_reflection;
	struct vk_rendertarg rt_refraction;
	texid_t tex_refraction;	//separate from rt_reflection, because $reasons
	texid_t tex_ripplemap;

	//descriptor sets are: 0) entity+light 1) batch textures + pass textures
	VkDescriptorSet descriptorsets[1];
//	VkDescriptorPool texturedescpool[2];

	//commandbuffer state, to avoid redundant state changes.
	VkPipeline activepipeline;

	struct shadowmaps_s
	{
		uint32_t width;
		uint32_t height;
		VkImage image;	//array. multiple allows for things to happen out of order, which should help to avoid barrier stalls.
		VkDeviceMemory memory;

		uint32_t seq;
		struct
		{
			VkFramebuffer framebuffer;
			image_t qimage;		//this is silly, but whatever.
			vk_image_t vimage;
		} buf[8];
	} shadow[2]; //omni, spot
	texid_t currentshadowmap;

	float depthrange;

	VkDescriptorSetLayout textureLayout;
} vkbackend_t;

#define VERTEXSTREAMSIZE (1024*1024*2)	//2mb = 1 PAE jumbo page

#define DYNVBUFFSIZE 65536
#define DYNIBUFFSIZE 65536

static vecV_t tmpbuf[65536];	//max verts per mesh

static vkbackend_t shaderstate;

extern int be_maxpasses;

struct blobheader
{
	unsigned char blobmagic[4];
	unsigned int blobversion;
	unsigned int defaulttextures;	//s_diffuse etc flags
	unsigned int numtextures;		//s_t0 count
	unsigned int permutations;		//

	unsigned int cvarsoffset;
	unsigned int cvarslength;

	unsigned int vertoffset;
	unsigned int vertlength;
	unsigned int fragoffset;
	unsigned int fraglength;
};

static float VK_ShaderReadArgument(const char *arglist, const char *arg, char type, qbyte size, void *out)
{
	qbyte i;
	const char *var;
	int arglen = strlen(arg);

	//grab an argument instead, otherwise 0
	var = arglist;
	while((var = strchr(var, '#')))
	{
		if (!Q_strncasecmp(var+1, arg, arglen))
		{
			if (var[1+arglen] == '=')
			{
				var = var+arglen+2;
				for (i = 0; i < size; i++)
				{
					while (*var == ' ' || *var == '\t' || *var == ',')
						var++;

					if (type == 'F')
						((float*)out)[i] = BigFloat(strtod(var, (char**)&var));
					else
						((int*)out)[i] = BigLong(strtol(var, (char**)&var, 0));
					if (!var)
						break;
				}
				return 1;
			}
			if (var[1+arglen] == '#' || !var[1+arglen])
			{
				for (i = 0; i < size; i++)
				{
					if (type == 'F')
						((float*)out)[i] = BigFloat(1);
					else
						((int*)out)[i] = BigLong(1);
				}
				return 1;	//present, but no value
			}
		}
		var++;
	}
	return 0;	//not present.
}

#if 0
//this should use shader pass flags, but those are specific to the shader, not the program, which makes this awkward.
static VkSampler VK_GetSampler(unsigned int flags)
{
	static VkSampler ret;
	qboolean clamptoedge = flags & IF_CLAMP;
	VkSamplerCreateInfo lmsampinfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	if (ret)
		return ret;

	if (flags & IF_LINEAR)
	{
		lmsampinfo.minFilter = lmsampinfo.magFilter = VK_FILTER_LINEAR;
		lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
	else if (flags & IF_NEAREST)
	{
		lmsampinfo.minFilter = lmsampinfo.magFilter = VK_FILTER_NEAREST;
		lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	}
	else
	{
		int *filter = (flags & IF_UIPIC)?vk.filterpic:vk.filtermip;
		if (filter[0])
			lmsampinfo.minFilter = VK_FILTER_LINEAR;
		else
			lmsampinfo.minFilter = VK_FILTER_NEAREST;
		if (filter[1])
			lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		else
			lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		if (filter[2])
			lmsampinfo.magFilter = VK_FILTER_LINEAR;
		else
			lmsampinfo.magFilter = VK_FILTER_NEAREST;
	}

	lmsampinfo.addressModeU = clamptoedge?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;
	lmsampinfo.addressModeV = clamptoedge?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;
	lmsampinfo.addressModeW = clamptoedge?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;
	lmsampinfo.mipLodBias = 0.0;
	lmsampinfo.anisotropyEnable = (flags & IF_NEAREST)?false:(vk.max_anistophy > 1);
	lmsampinfo.maxAnisotropy = vk.max_anistophy;
	lmsampinfo.compareEnable = VK_FALSE;
	lmsampinfo.compareOp = VK_COMPARE_OP_NEVER;
	lmsampinfo.minLod = vk.mipcap[0];	//this isn't quite right
	lmsampinfo.maxLod = vk.mipcap[1];
	lmsampinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	lmsampinfo.unnormalizedCoordinates = VK_FALSE;
	VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &ret));

	return ret;
}
#endif

//creates the layout stuff for the prog.
static void VK_FinishProg(program_t *prog, const char *name)
{
	{
		VkDescriptorSetLayout desclayout;
		VkDescriptorSetLayoutCreateInfo descSetLayoutCreateInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		VkDescriptorSetLayoutBinding dbs[2+MAX_TMUS], *db = dbs;
		uint32_t i;
		//VkSampler samp = VK_GetSampler(0);

		db->binding = db-dbs;
		db->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		db->descriptorCount = 1;
		db->stageFlags = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
		db->pImmutableSamplers = NULL;
		db++;

		db->binding = db-dbs;
		db->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		db->descriptorCount = 1;
		db->stageFlags = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
		db->pImmutableSamplers = NULL;
		db++;

		for (i = 0; i < 32; i++)
		{
			if (!(prog->defaulttextures & (1u<<i)))
				continue;
			db->binding = db-dbs;
			db->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			db->descriptorCount = 1;
			db->stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			db->pImmutableSamplers = NULL;//&samp;
			db++;
		}

		for (i = 0; i < prog->numsamplers; i++)
		{
			db->binding = db-dbs;
			db->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			db->descriptorCount = 1;
			db->stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			db->pImmutableSamplers = NULL;//&samp;
			db++;
		}

		descSetLayoutCreateInfo.bindingCount = db-dbs;
		descSetLayoutCreateInfo.pBindings = dbs;
		VkAssert(vkCreateDescriptorSetLayout(vk.device, &descSetLayoutCreateInfo, NULL, &desclayout));
		prog->desclayout = desclayout;
	}

	{
		VkDescriptorSetLayout sets[1] = {prog->desclayout};
		VkPipelineLayout layout;
		VkPushConstantRange push[1];
		VkPipelineLayoutCreateInfo pipeLayoutCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		push[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		push[0].offset = 0;
		push[0].size = sizeof(vec4_t);

		pipeLayoutCreateInfo.flags = 0;
		pipeLayoutCreateInfo.setLayoutCount = countof(sets);
		pipeLayoutCreateInfo.pSetLayouts = sets;
		pipeLayoutCreateInfo.pushConstantRangeCount = !strncmp(name, "fixedemu", 8);
		pipeLayoutCreateInfo.pPushConstantRanges = push;
		VkAssert(vkCreatePipelineLayout(vk.device, &pipeLayoutCreateInfo, vkallocationcb, &layout));
		prog->layout = layout;
	}
}


static const char *vulkan_glsl_hdrs[] =
{
	"sys/defs.h",
			"#define DEFS_DEFINED\n"
			"#undef texture2D\n"	//nvidia is fucking us over
			"#undef textureCube\n"	//nvidia is fucking us over
			"#define texture2D texture\n"
			"#define textureCube texture\n"
			"#define e_lmscale e_lmscales[0]\n"
		,
	"sys/skeletal.h",
			"#ifdef SKELETAL\n"
				"vec4 skeletaltransform()"
				"{"
					"mat3x4 wmat;\n"
					"wmat = m_bones[int(v_bone.x)] * v_weight.x;\n"
					"wmat += m_bones[int(v_bone.y)] * v_weight.y;\n"
					"wmat += m_bones[int(v_bone.z)] * v_weight.z;\n"
					"wmat += m_bones[int(v_bone.w)] * v_weight.w;\n"
					"return m_modelviewprojection * vec4(vec4(v_position.xyz, 1.0) * wmat, 1.0);"
				"}\n"
				"vec4 skeletaltransform_nst(out vec3 n, out vec3 t, out vec3 b)"
				"{"
					"mat3x4 wmat;\n"
					"wmat = m_bones[int(v_bone.x)] * v_weight.x;"
					"wmat += m_bones[int(v_bone.y)] * v_weight.y;"
					"wmat += m_bones[int(v_bone.z)] * v_weight.z;"
					"wmat += m_bones[int(v_bone.w)] * v_weight.w;"
					"n = vec4(v_normal.xyz, 0.0) * wmat;"
					"t = vec4(v_svector.xyz, 0.0) * wmat;"
					"b = vec4(v_tvector.xyz, 0.0) * wmat;"
					"return m_modelviewprojection * vec4(vec4(v_position.xyz, 1.0) * wmat, 1.0);"
				"}\n"
				"vec4 skeletaltransform_wnst(out vec3 w, out vec3 n, out vec3 t, out vec3 b)"
				"{"
					"mat3x4 wmat;\n"
					"wmat = m_bones[int(v_bone.x)] * v_weight.x;"
					"wmat += m_bones[int(v_bone.y)] * v_weight.y;"
					"wmat += m_bones[int(v_bone.z)] * v_weight.z;"
					"wmat += m_bones[int(v_bone.w)] * v_weight.w;"
					"n = vec4(v_normal.xyz, 0.0) * wmat;"
					"t = vec4(v_svector.xyz, 0.0) * wmat;"
					"b = vec4(v_tvector.xyz, 0.0) * wmat;"
					"w = vec4(v_position.xyz, 1.0) * wmat;"
					"return m_modelviewprojection * vec4(w, 1.0);"
				"}\n"
				"vec4 skeletaltransform_n(out vec3 n)"
				"{"
					"mat3x4 wmat;\n"
					"wmat = m_bones[int(v_bone.x)] * v_weight.x;"
					"wmat += m_bones[int(v_bone.y)] * v_weight.y;"
					"wmat += m_bones[int(v_bone.z)] * v_weight.z;"
					"wmat += m_bones[int(v_bone.w)] * v_weight.w;"
					"n = vec4(v_normal.xyz, 0.0) * wmat;"
					"return m_modelviewprojection * vec4(vec4(v_position.xyz, 1.0) * wmat, 1.0);"
				"}\n"
			"#else\n"
				"#define skeletaltransform ftetransform\n"
				"vec4 skeletaltransform_wnst(out vec3 w, out vec3 n, out vec3 t, out vec3 b)"
				"{"
					"n = v_normal;"
					"t = v_svector;"
					"b = v_tvector;"
					"w = v_position.xyz;"
					"return ftetransform();"
				"}\n"
				"vec4 skeletaltransform_nst(out vec3 n, out vec3 t, out vec3 b)"
				"{"
					"n = v_normal;"
					"t = v_svector;"
					"b = v_tvector;"
					"return ftetransform();"
				"}\n"
				"vec4 skeletaltransform_n(out vec3 n)"
				"{"
					"n = v_normal;"
					"return ftetransform();"
				"}\n"
			"#endif\n"
		,
	"sys/fog.h",
			"#ifdef FRAGMENT_SHADER\n"
				"#ifdef FOG\n"
					"vec3 fog3(in vec3 regularcolour)"
					"{"
						"float z = w_fogdensity * gl_FragCoord.z / gl_FragCoord.w;\n"
						"z = max(0.0,z-w_fogdepthbias);\n"
						"#if #include \"cvar/r_fog_exp2\"\n"
						"z *= z;\n"
						"#endif\n"
						"float fac = exp2(-(z * 1.442695));\n"
						"fac = (1.0-w_fogalpha) + (clamp(fac, 0.0, 1.0)*w_fogalpha);\n"
						"return mix(w_fogcolour, regularcolour, fac);\n"
					"}\n"
					"vec3 fog3additive(in vec3 regularcolour)"
					"{"
						"float z = w_fogdensity * gl_FragCoord.z / gl_FragCoord.w;\n"
						"z = max(0.0,z-w_fogdepthbias);\n"
						"#if #include \"cvar/r_fog_exp2\"\n"
						"z *= z;\n"
						"#endif\n"
						"float fac = exp2(-(z * 1.442695));\n"
						"fac = (1.0-w_fogalpha) + (clamp(fac, 0.0, 1.0)*w_fogalpha);\n"
						"return regularcolour * fac;\n"
					"}\n"
					"vec4 fog4(in vec4 regularcolour)"
					"{"
						"return vec4(fog3(regularcolour.rgb), 1.0) * regularcolour.a;\n"
					"}\n"
					"vec4 fog4additive(in vec4 regularcolour)"
					"{"
						"float z = w_fogdensity * gl_FragCoord.z / gl_FragCoord.w;\n"
						"z = max(0.0,z-w_fogdepthbias);\n"
						"#if #include \"cvar/r_fog_exp2\"\n"
						"z *= z;\n"
						"#endif\n"
						"float fac = exp2(-(z * 1.442695));\n"
						"fac = (1.0-w_fogalpha) + (clamp(fac, 0.0, 1.0)*w_fogalpha);\n"
						"return regularcolour * vec4(fac, fac, fac, 1.0);\n"
					"}\n"
					"vec4 fog4blend(in vec4 regularcolour)"
					"{"
						"float z = w_fogdensity * gl_FragCoord.z / gl_FragCoord.w;\n"
						"z = max(0.0,z-w_fogdepthbias);\n"
						"#if #include \"cvar/r_fog_exp2\"\n"
						"z *= z;\n"
						"#endif\n"
						"float fac = exp2(-(z * 1.442695));\n"
						"fac = (1.0-w_fogalpha) + (clamp(fac, 0.0, 1.0)*w_fogalpha);\n"
						"return regularcolour * vec4(1.0, 1.0, 1.0, fac);\n"
					"}\n"
				"#else\n"
					/*don't use macros for this - mesa bugs out*/
					"vec3 fog3(in vec3 regularcolour) { return regularcolour; }\n"
					"vec3 fog3additive(in vec3 regularcolour) { return regularcolour; }\n"
					"vec4 fog4(in vec4 regularcolour) { return regularcolour; }\n"
					"vec4 fog4additive(in vec4 regularcolour) { return regularcolour; }\n"
					"vec4 fog4blend(in vec4 regularcolour) { return regularcolour; }\n"
				"#endif\n"
			"#endif\n"
		,
	"sys/offsetmapping.h",
			"uniform float cvar_r_glsl_offsetmapping_scale;\n"
			"vec2 offsetmap(sampler2D normtex, vec2 base, vec3 eyevector)\n"
			"{\n"
			"#if !defined(OFFSETMAPPING_SCALE)\n"
				"#define OFFSETMAPPING_SCALE 1.0\n"
			"#endif\n"
			"#if defined(RELIEFMAPPING) && !defined(GL_ES)\n"
				"float i, f;\n"
				"vec3 OffsetVector = vec3(normalize(eyevector.xyz).xy * cvar_r_glsl_offsetmapping_scale * OFFSETMAPPING_SCALE * vec2(-1.0, 1.0), -1.0);\n"
				"vec3 RT = vec3(vec2(base.xy"/* - OffsetVector.xy*OffsetMapping_Bias*/"), 1.0);\n"
				"OffsetVector /= 10.0;\n"
				"for(i = 1.0; i < 10.0; ++i)\n"
					"RT += OffsetVector *  step(texture2D(normtex, RT.xy).a, RT.z);\n"
				"for(i = 0.0, f = 1.0; i < 5.0; ++i, f *= 0.5)\n"
					"RT += OffsetVector * (step(texture2D(normtex, RT.xy).a, RT.z) * f - 0.5 * f);\n"
				"return RT.xy;\n"
			"#elif defined(OFFSETMAPPING)\n"
				"vec2 OffsetVector = normalize(eyevector).xy * cvar_r_glsl_offsetmapping_scale * OFFSETMAPPING_SCALE * vec2(-1.0, 1.0);\n"
				"vec2 tc = base;\n"
				"tc += OffsetVector;\n"
				"OffsetVector *= 0.333;\n"
				"tc -= OffsetVector * texture2D(normtex, tc).w;\n"
				"tc -= OffsetVector * texture2D(normtex, tc).w;\n"
				"tc -= OffsetVector * texture2D(normtex, tc).w;\n"
				"return tc;\n"
			"#else\n"
				"return base;\n"
			"#endif\n"
			"}\n"
		,
	"sys/pcf.h",
			"#ifndef r_glsl_pcf\n"
				"#define r_glsl_pcf 9\n"
			"#endif\n"
			"#if r_glsl_pcf < 1\n"
				"#undef r_glsl_pcf\n"
				"#define r_glsl_pcf 9\n"
			"#endif\n"
			"vec3 ShadowmapCoord(void)\n"
			"{\n"
			"#ifdef SPOT\n"
				//bias it. don't bother figuring out which side or anything, its not needed
				//l_projmatrix contains the light's projection matrix so no other magic needed
				"return ((vtexprojcoord.xyz-vec3(0.0,0.0,0.015))/vtexprojcoord.w + vec3(1.0, 1.0, 1.0)) * vec3(0.5, 0.5, 0.5);\n"
			//"#elif defined(CUBESHADOW)\n"
			//	vec3 shadowcoord = vshadowcoord.xyz / vshadowcoord.w;
			//	#define dosamp(x,y) shadowCube(s_t4, shadowcoord + vec2(x,y)*texscale.xy).r
			"#else\n"
				//figure out which axis to use
				//texture is arranged thusly:
				//forward left  up
				//back    right down
				"vec3 dir = abs(vtexprojcoord.xyz);\n"
				//assume z is the major axis (ie: forward from the light)
				"vec3 t = vtexprojcoord.xyz;\n"
				"float ma = dir.z;\n"
				"vec3 axis = vec3(0.5/3.0, 0.5/2.0, 0.5);\n"
				"if (dir.x > ma)\n"
				"{\n"
					"ma = dir.x;\n"
					"t = vtexprojcoord.zyx;\n"
					"axis.x = 0.5;\n"
				"}\n"
				"if (dir.y > ma)\n"
				"{\n"
					"ma = dir.y;\n"
					"t = vtexprojcoord.xzy;\n"
					"axis.x = 2.5/3.0;\n"
				"}\n"
				//if the axis is negative, flip it.
				"if (t.z > 0.0)\n"
				"{\n"
					"axis.y = 1.5/2.0;\n"
					"t.z = -t.z;\n"
				"}\n"

				//we also need to pass the result through the light's projection matrix too
				//the 'matrix' we need only contains 5 actual values. and one of them is a -1. So we might as well just use a vec4.
				//note: the projection matrix also includes scalers to pinch the image inwards to avoid sampling over borders, as well as to cope with non-square source image
				//the resulting z is prescaled to result in a value between -0.5 and 0.5.
				//also make sure we're in the right quadrant type thing
				"return axis + ((l_shadowmapproj.xyz*t.xyz + vec3(0.0, 0.0, l_shadowmapproj.w)) / -t.z);\n"
			"#endif\n"
			"}\n"

			"float ShadowmapFilter(sampler2DShadow smap)\n"
			"{\n"
				"vec3 shadowcoord = ShadowmapCoord();\n"

				"#if 0\n"//def GL_ARB_texture_gather
					"vec2 ipart, fpart;\n"
					"#define dosamp(x,y) textureGatherOffset(smap, ipart.xy, vec2(x,y)))\n"
					"vec4 tl = step(shadowcoord.z, dosamp(-1.0, -1.0));\n"
					"vec4 bl = step(shadowcoord.z, dosamp(-1.0, 1.0));\n"
					"vec4 tr = step(shadowcoord.z, dosamp(1.0, -1.0));\n"
					"vec4 br = step(shadowcoord.z, dosamp(1.0, 1.0));\n"
					//we now have 4*4 results, woo
					//we can just average them for 1/16th precision, but that's still limited graduations
					//the middle four pixels are 'full strength', but we interpolate the sides to effectively give 3*3
					"vec4 col =     vec4(tl.ba, tr.ba) + vec4(bl.rg, br.rg) + " //middle two rows are full strength
							"mix(vec4(tl.rg, tr.rg), vec4(bl.ba, br.ba), fpart.y);\n" //top+bottom rows
					"return dot(mix(col.rgb, col.agb, fpart.x), vec3(1.0/9.0));\n"	//blend r+a, gb are mixed because its pretty much free and gives a nicer dot instruction instead of lots of adds.
				"#else\n"
					"#define dosamp(x,y) shadow2D(smap, shadowcoord.xyz + (vec3(x,y,0.0)*l_shadowmapscale.xyx)).r\n"
					"float s = 0.0;\n"
					"#if r_glsl_pcf >= 1 && r_glsl_pcf < 5\n"
						"s += dosamp(0.0, 0.0);\n"
						"return s;\n"
					"#elif r_glsl_pcf >= 5 && r_glsl_pcf < 9\n"
						"s += dosamp(-1.0, 0.0);\n"
						"s += dosamp(0.0, -1.0);\n"
						"s += dosamp(0.0, 0.0);\n"
						"s += dosamp(0.0, 1.0);\n"
						"s += dosamp(1.0, 0.0);\n"
						"return s/5.0;\n"
					"#else\n"
						"s += dosamp(-1.0, -1.0);\n"
						"s += dosamp(-1.0, 0.0);\n"
						"s += dosamp(-1.0, 1.0);\n"
						"s += dosamp(0.0, -1.0);\n"
						"s += dosamp(0.0, 0.0);\n"
						"s += dosamp(0.0, 1.0);\n"
						"s += dosamp(1.0, -1.0);\n"
						"s += dosamp(1.0, 0.0);\n"
						"s += dosamp(1.0, 1.0);\n"
						"return s/9.0;\n"
					"#endif\n"
				"#endif\n"
			"}\n"
		,
	NULL
};

//glsl doesn't officially support #include, this might be vulkan, but don't push things.
qboolean Vulkan_GenerateIncludes(int maxstrings, int *strings, const char *prstrings[], int length[], const char *shadersource)
{
	int i;
	char *incline, *inc;
	char incname[256];
	while((incline=strstr(shadersource, "#include")))
	{
		if (*strings == maxstrings)
			return false;

		/*emit up to the include*/
		if (incline - shadersource)
		{
			prstrings[*strings] = shadersource;
			length[*strings] = incline - shadersource;
			*strings += 1;
		}

		incline += 8;
		incline = COM_ParseOut (incline, incname, sizeof(incname));

		if (!strncmp(incname, "cvar/", 5))
		{
			cvar_t *var = Cvar_Get(incname+5, "0", 0, "shader cvars");
			if (var)
			{
				var->flags |= CVAR_SHADERSYSTEM;
				if (!Vulkan_GenerateIncludes(maxstrings, strings, prstrings, length, var->string))
					return false;
			}
			else
			{
				/*dump something if the cvar doesn't exist*/
				if (*strings == maxstrings)
					return false;
				prstrings[*strings] = "0";
				length[*strings] = strlen("0");
				*strings += 1;
			}
		}
		else
		{
			for (i = 0; vulkan_glsl_hdrs[i]; i += 2)
			{
				if (!strcmp(incname, vulkan_glsl_hdrs[i]))
				{
					if (!Vulkan_GenerateIncludes(maxstrings, strings, prstrings, length, vulkan_glsl_hdrs[i+1]))
						return false;
					break;
				}
			}
			if (!vulkan_glsl_hdrs[i])
			{
				if (FS_LoadFile(incname, (void**)&inc) != (qofs_t)-1)
				{
					if (!Vulkan_GenerateIncludes(maxstrings, strings, prstrings, length, inc))
					{
						FS_FreeFile(inc);
						return false;
					}
					FS_FreeFile(inc);
				}
			}
		}

		/*move the pointer past the include*/
		shadersource = incline;
	}
	if (*shadersource)
	{
		if (*strings == maxstrings)
			return false;

		/*dump the remaining shader string*/
		prstrings[*strings] = shadersource;
		length[*strings] = strlen(prstrings[*strings]);
		*strings += 1;
	}
	return true;
}

//assumes VK_NV_glsl_shader for raw glsl
VkShaderModule VK_CreateGLSLModule(program_t *prog, const char *name, int ver, const char **precompilerconstants, const char *body, int isfrag)
{
	VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	VkShaderModule mod;
	const char *strings[256];
	int lengths[256];
	unsigned int numstrings = 0;
	char *blob;
	size_t blobsize;
	unsigned int i;

	strings[numstrings++] = "#version 450 core\n";
	strings[numstrings++] = "#define ENGINE_"DISTRIBUTION"\n";

	strings[numstrings++] = 
"layout(std140, binding=0) uniform entityblock"
"{\n"
	"mat4 m_modelviewproj;"
	"mat4 m_model;"
	"mat4 m_modelinv;"
	"vec3 e_eyepos;"
	"float e_time;"
	"vec3 e_light_ambient;	float epad1;"
	"vec3 e_light_dir;	float epad2;"
	"vec3 e_light_mul;	float epad3;"
	"vec4 e_lmscales[4];"
	"vec3 e_uppercolour;	float epad4;"
	"vec3 e_lowercolour;	float epad5;"
	"vec4 e_colourident;"
	"vec4 w_fogcolours;"
	"float w_fogdensity;	float w_fogdepthbias;	 vec2 epad6;"
"};\n"

"layout(std140, binding=1) uniform lightblock"
"{\n"
	"mat4 l_cubematrix;"
	"vec3 l_lightposition; 	float lpad1;"
	"vec3 l_lightcolour; 		float lpad2;"
	"vec3 l_lightcolourscale; 	float l_lightradius;"
	"vec4 l_shadowmapproj;"
	"vec2 l_shadowmapscale;	vec2 lpad3;"
"};\n"
;

	if (isfrag)
	{
		int bindloc = 0;
		const char *bindlocations[] =
		{
			"layout(set=0, binding=2) ",
			"layout(set=0, binding=3) ",
			"layout(set=0, binding=4) ",
			"layout(set=0, binding=5) ",
			"layout(set=0, binding=6) ",
			"layout(set=0, binding=7) ",
			"layout(set=0, binding=8) ",
			"layout(set=0, binding=9) ",
			"layout(set=0, binding=10) ",
			"layout(set=0, binding=11) ",
			"layout(set=0, binding=12) ",
			"layout(set=0, binding=13) ",
			"layout(set=0, binding=14) ",
			"layout(set=0, binding=15) ",
			"layout(set=0, binding=16) ",
			"layout(set=0, binding=17) ",
			"layout(set=0, binding=18) ",
			"layout(set=0, binding=19) ",
			"layout(set=0, binding=20) ",
			"layout(set=0, binding=21) ",
			"layout(set=0, binding=22) ",
			"layout(set=0, binding=23) ",
			"layout(set=0, binding=24) ",
			"layout(set=0, binding=25) ",
		};
		const char *numberedsamplernames[] =
		{
			"uniform sampler2D s_t0;\n",
			"uniform sampler2D s_t1;\n",
			"uniform sampler2D s_t2;\n",
			"uniform sampler2D s_t3;\n",
			"uniform sampler2D s_t4;\n",
			"uniform sampler2D s_t5;\n",
			"uniform sampler2D s_t6;\n",
			"uniform sampler2D s_t7;\n",
		};
		const char *defaultsamplernames[] =
		{
			"uniform sampler2D s_shadowmap;\n",
			"uniform samplerCube s_projectionmap;\n",
			"uniform sampler2D s_diffuse;\n",
			"uniform sampler2D s_normalmap;\n",
			"uniform sampler2D s_specular;\n",
			"uniform sampler2D s_upper;\n",
			"uniform sampler2D s_lower;\n",
			"uniform sampler2D s_fullbright;\n",
			"uniform sampler2D s_paletted;\n",
			"uniform samplerCube s_reflectcube;\n",
			"uniform sampler2D s_reflectmask;\n",
			"uniform sampler2D s_lightmap;\n#define s_lightmap0 s_lightmap\n",
			"uniform sampler2D s_deluxmap;\n#define s_deluxmap0 s_deluxmap\n",

			"uniform sampler2D s_lightmap1;\n",
			"uniform sampler2D s_lightmap2;\n",
			"uniform sampler2D s_lightmap3;\n",
			"uniform sampler2D s_deluxmap1;\n",
			"uniform sampler2D s_deluxmap2;\n",
			"uniform sampler2D s_deluxmap3;\n",
		};

		strings[numstrings++] = "#define FRAGMENT_SHADER\n"
"#define varying in\n"
"layout(location=0) out vec4 outcolour;\n"
"#define gl_FragColor outcolour\n"
;

		for (i = 0; i < countof(defaultsamplernames); i++)
		{
			if (prog->defaulttextures & (1u<<i))
			{
				strings[numstrings++] = bindlocations[bindloc++];
				strings[numstrings++] = defaultsamplernames[i];
			}
		}
		for (i = 0; i < prog->numsamplers && i < countof(numberedsamplernames); i++)
		{
			strings[numstrings++] = bindlocations[bindloc++];
			strings[numstrings++] = numberedsamplernames[i];
		}
	}
	else
	{
		strings[numstrings++] = "#define VERTEX_SHADER\n"
"#define attribute in\n"
"#define varying out\n"
"out gl_PerVertex"
"{"
  "vec4 gl_Position;"
"};"


"layout(location=0) attribute vec3 v_position;"
"layout(location=1) attribute vec2 v_texcoord;"
"layout(location=2) attribute vec4 v_colour;"
"layout(location=3) attribute vec2 v_lmcoord;"
"layout(location=4) attribute vec3 v_normal;"
"layout(location=5) attribute vec3 v_svector;"
"layout(location=6) attribute vec3 v_tvector;"
//"layout(location=7) attribute vec4 v_boneweights;"
//"layout(location=8) attribute ivec4 v_bonenums;"

"\n"

"vec4 ftetransform()"
"{"
	"vec4 proj = (m_modelviewproj*vec4(v_position,1.0));"
	"proj.y *= -1;"
	"proj.z = (proj.z + proj.w) / 2.0;"
	"return proj;"
"}\n"
;
	}

	while (*precompilerconstants)
		strings[numstrings++] = *precompilerconstants++;

	for (i = 0, blobsize = 0; i < numstrings; i++)
		lengths[i] = strlen(strings[i]);
	Vulkan_GenerateIncludes(countof(strings), &numstrings, strings, lengths, body);

	//now glue it all together into a single blob
	for (i = 0, blobsize = 0; i < numstrings; i++)
		blobsize += lengths[i];
	blobsize++;
	blob = malloc(blobsize);
	for (i = 0, blobsize = 0; i < numstrings; i++)
	{
		memcpy(blob+blobsize, strings[i], lengths[i]);
		blobsize += lengths[i];
	}
	blob[blobsize] = 0;

	//and submit it.
	info.flags = 0;
	info.codeSize = blobsize;
	info.pCode = (void*)blob;
	VkAssert(vkCreateShaderModule(vk.device, &info, vkallocationcb, &mod));
	return mod;
}

qboolean VK_LoadGLSL(program_t *prog, const char *name, unsigned int permu, int ver, const char **precompilerconstants, const char *vert, const char *tcs, const char *tes, const char *geom, const char *frag, qboolean noerrors, vfsfile_t *blobfile)
{
	if (permu)	//FIXME...
		return false;

	prog->nofixedcompat = false;
//	prog->supportedpermutations = 0;
	prog->cvardata = NULL;
	prog->cvardatasize = 0;
	prog->pipelines = NULL;
	prog->vert = VK_CreateGLSLModule(prog, name, ver, precompilerconstants, vert, false);
	prog->frag = VK_CreateGLSLModule(prog, name, ver, precompilerconstants, frag, true);

	VK_FinishProg(prog, name);

	return true;
}

qboolean VK_LoadBlob(program_t *prog, void *blobdata, const char *name)
{
	//fixme: should validate that the offset+lengths are within the blobdata.
	struct blobheader *blob = blobdata;
	VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	VkShaderModule vert, frag;
	unsigned char *cvardata;

	if (blob->blobmagic[0] != 0xff || blob->blobmagic[1] != 'S' || blob->blobmagic[2] != 'P' || blob->blobmagic[3] != 'V')
		return false;	//assume its glsl. this is going to be 'fun'.
	if (blob->blobversion != 1)
	{
		Con_Printf("Blob %s is outdated\n", name);
		return false;
	}

	info.flags = 0;
	info.codeSize = blob->vertlength;
	info.pCode = (uint32_t*)((char*)blob+blob->vertoffset);
	VkAssert(vkCreateShaderModule(vk.device, &info, vkallocationcb, &vert));

	info.flags = 0;
	info.codeSize = blob->fraglength;
	info.pCode = (uint32_t*)((char*)blob+blob->fragoffset);
	VkAssert(vkCreateShaderModule(vk.device, &info, vkallocationcb, &frag));

	prog->vert = vert;
	prog->frag = frag;
	prog->nofixedcompat = true;
	prog->numsamplers = blob->numtextures;
	prog->defaulttextures = blob->defaulttextures;
	prog->supportedpermutations = blob->permutations;

	if (blob->cvarslength)
	{
		prog->cvardata = BZ_Malloc(blob->cvarslength);
		prog->cvardatasize = blob->cvarslength;
		memcpy(prog->cvardata, (char*)blob+blob->cvarsoffset, blob->cvarslength);
	}
	else
	{
		prog->cvardata = NULL;
		prog->cvardatasize = 0;
	}

	//go through the cvars and a) validate them. b) create them with the right defaults.
	//FIXME: validate
	for (cvardata = prog->cvardata; cvardata < prog->cvardata + prog->cvardatasize; )
	{
		unsigned char type = cvardata[2], size = cvardata[3]-'0';
		char *cvarname;
		cvar_t *var;

		cvardata += 4;
		cvarname = cvardata;
		cvardata += strlen(cvarname)+1;

		if (type >= 'A' && type <= 'Z')
		{	//args will be handled by the blob loader.
			VK_ShaderReadArgument(name, cvarname, type, size, cvardata);
		}
		else
		{
			var = Cvar_FindVar(cvarname);
			if (var)
				var->flags |= CVAR_SHADERSYSTEM;	//just in case
			else
			{
				union
				{
					int i;
					float f;
				} u;
				char value[128];
				uint32_t i;
				*value = 0;
				for (i = 0; i < size; i++)
				{
					u.i = (cvardata[i*4+0]<<24)|(cvardata[i*4+1]<<16)|(cvardata[i*4+2]<<8)|(cvardata[i*4+3]<<0);
					if (i)
						Q_strncatz(value, " ", sizeof(value));
					if (type == 'i' || type == 'b')
						Q_strncatz(value, va("%i", u.i), sizeof(value));
					else
						Q_strncatz(value, va("%f", u.f), sizeof(value));
				}
				Cvar_Get(cvarname, value, CVAR_SHADERSYSTEM, "GLSL Settings");
			}
		}
		cvardata += 4*size;
	}

	VK_FinishProg(prog, name);

	prog->pipelines = NULL;	//generated as needed, depending on blend states etc.
	return true;
}
void VKBE_DeleteProg(program_t *prog)
{
	struct pipeline_s *pipe;
	Z_Free(prog->cvardata);
	while(prog->pipelines)
	{
		pipe = prog->pipelines;
		prog->pipelines = pipe->next;

		if (pipe->pipeline)
			vkDestroyPipeline(vk.device, pipe->pipeline, vkallocationcb);
		Z_Free(pipe);
	}
	if (prog->layout)
		vkDestroyPipelineLayout(vk.device, prog->layout, vkallocationcb);
	prog->layout = VK_NULL_HANDLE;
	if (prog->desclayout)
		vkDestroyDescriptorSetLayout(vk.device, prog->desclayout, vkallocationcb);
	prog->desclayout = VK_NULL_HANDLE;
	if (prog->vert)
		vkDestroyShaderModule(vk.device, prog->vert, vkallocationcb);
	prog->vert = VK_NULL_HANDLE;
	if (prog->frag)
		vkDestroyShaderModule(vk.device, prog->frag, vkallocationcb);
	prog->frag = VK_NULL_HANDLE;
}

static unsigned int VKBE_ApplyShaderBits(unsigned int bits)
{
	if (shaderstate.flags & (BEF_FORCEADDITIVE|BEF_FORCETRANSPARENT|BEF_FORCENODEPTH|BEF_FORCEDEPTHTEST|BEF_FORCEDEPTHWRITE|BEF_LINES))
	{
		if (shaderstate.flags & BEF_FORCEADDITIVE)
			bits = (bits & ~(SBITS_MISC_DEPTHWRITE|SBITS_BLEND_BITS|SBITS_ATEST_BITS))
						| (SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE);
		else if (shaderstate.flags & BEF_FORCETRANSPARENT)
		{
			if ((bits & SBITS_BLEND_BITS) == (SBITS_SRCBLEND_ONE|SBITS_DSTBLEND_ZERO) || !(bits & SBITS_BLEND_BITS) || (bits&SBITS_ATEST_GE128)) 	/*if transparency is forced, clear alpha test bits*/
				bits = (bits & ~(SBITS_MISC_DEPTHWRITE|SBITS_BLEND_BITS|SBITS_ATEST_BITS))
							| (SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}

		if (shaderstate.flags & BEF_FORCENODEPTH) 	/*EF_NODEPTHTEST dp extension*/
			bits |= SBITS_MISC_NODEPTHTEST;
		else
		{
			if (shaderstate.flags & BEF_FORCEDEPTHTEST)
				bits &= ~SBITS_MISC_NODEPTHTEST;
			if (shaderstate.flags & BEF_FORCEDEPTHWRITE)
				bits |= SBITS_MISC_DEPTHWRITE;
		}

		if (shaderstate.flags & BEF_LINES)
			bits |= SBITS_LINES;
	}
	return bits;
}

static const char LIGHTPASS_SHADER[] = "\
{\n\
	program rtlight\n\
	{\n\
		blendfunc add\n\
	}\n\
}";

void VKBE_Init(void)
{
	int i;
	char *c;

	sh_config.pDeleteProg = VKBE_DeleteProg;

	be_maxpasses = 1;
	memset(&shaderstate, 0, sizeof(shaderstate));
	shaderstate.inited = true;
	shaderstate.curvertdecl = -1;
	for (i = 0; i < MAXRLIGHTMAPS; i++)
		shaderstate.dummybatch.lightmap[i] = -1;

	shaderstate.identitylighting = 1;
	shaderstate.identitylightmap = 1;

	//make sure the world draws correctly
	r_worldentity.shaderRGBAf[0] = 1;
	r_worldentity.shaderRGBAf[1] = 1;
	r_worldentity.shaderRGBAf[2] = 1;
	r_worldentity.shaderRGBAf[3] = 1;
	r_worldentity.axis[0][0] = 1;
	r_worldentity.axis[1][1] = 1;
	r_worldentity.axis[2][2] = 1;
	r_worldentity.light_avg[0] = 1;
	r_worldentity.light_avg[1] = 1;
	r_worldentity.light_avg[2] = 1;

	FTable_Init();

	{
		unsigned char bibuf[4*4*4] = {0};
		if (!qrenderer)
			r_blackimage = r_nulltex;
		else
			r_blackimage = R_LoadTexture("$blackimage", 4, 4, TF_RGBA32, bibuf, IF_NOMIPMAP|IF_NOPICMIP|IF_NEAREST|IF_NOGAMMA);
	}

	shaderstate.depthonly = R_RegisterShader("depthonly", SUF_NONE, 
				"{\n"
					"program depthonly\n"
					"{\n"
						"depthwrite\n"
						"maskcolor\n"
					"}\n"
				"}\n");


	shaderstate.programfixedemu[0] = Shader_FindGeneric("fixedemu", QR_VULKAN);
	shaderstate.programfixedemu[1] = Shader_FindGeneric("fixedemu#CONSTCOLOUR", QR_VULKAN);

	R_InitFlashblends();

/*
	{
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		VkDescriptorPoolSize dpisz[2];
		dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		dpi.maxSets = 512;
		dpi.poolSizeCount = countof(dpisz);
		dpi.pPoolSizes = dpisz;

		dpisz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		dpisz[0].descriptorCount = 2;

		dpisz[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		dpisz[1].descriptorCount = MAX_TMUS;

		VkAssert(vkCreateDescriptorPool(vk.device, &dpi, NULL, &shaderstate.texturedescpool));
	}
*/
	{
		struct stagingbuf lazybuf;
		void *buffer = VKBE_CreateStagingBuffer(&lazybuf, sizeof(vec4_t)*65536+sizeof(vec3_t)*3*65536, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
		vec4_t *col = buffer;
		vec3_t *norm = (vec3_t*)(col+65536);
		vec3_t *sdir = norm+65536;
		vec3_t *tdir = sdir+65536;
		for (i = 0; i < 65536; i++)
		{
			Vector4Set(col[i], 1, 1, 1, 1);
			VectorSet(norm[i], 1, 0, 0);
			VectorSet(sdir[i], 0, 1, 0);
			VectorSet(tdir[i], 0, 0, 1);
		}
		shaderstate.staticbuf = VKBE_FinishStaging(&lazybuf, &shaderstate.staticbufmem);
	}


	c = vk_stagingbuffers.string;
	if (*c)
	{
		vk_usedynamicstaging = 0;
		while (*c)
		{
			if (*c == 'u')
				vk_usedynamicstaging |= 1u<<DB_UBO;
			else if (*c == 'e' || *c == 'i')
				vk_usedynamicstaging |= 1u<<DB_EBO;
			else if (*c == 'v')
				vk_usedynamicstaging |= 1u<<DB_VBO;
			else if (*c == '0')
				vk_usedynamicstaging |= 0;	//for explicly none.
			else
				Con_Printf("%s: unknown char %c\n", vk_stagingbuffers.string, *c);
			c++;
		}
	}
	else
		vk_usedynamicstaging = 0u;
}

static struct descpool *VKBE_CreateDescriptorPool(void)
{
	struct descpool *np = Z_Malloc(sizeof(*np));
	
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	VkDescriptorPoolSize dpisz[2];
	dpi.flags = 0;
	dpi.maxSets = np->totalsets = 512;
	dpi.poolSizeCount = countof(dpisz);
	dpi.pPoolSizes = dpisz;

	dpisz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	dpisz[0].descriptorCount = 2*dpi.maxSets;

	dpisz[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	dpisz[1].descriptorCount = MAX_TMUS*dpi.maxSets;

	VkAssert(vkCreateDescriptorPool(vk.device, &dpi, NULL, &np->pool));

	return np;
}
static VkDescriptorSet VKBE_TempDescriptorSet(VkDescriptorSetLayout layout)
{
	VkDescriptorSet ret;
	VkDescriptorSetAllocateInfo setinfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

	if (vk.descpool->availsets == 0)
	{
		if (vk.descpool->next)
			vk.descpool = vk.descpool->next;
		else
			vk.descpool = vk.descpool->next = VKBE_CreateDescriptorPool();
		vkResetDescriptorPool(vk.device, vk.descpool->pool, 0);
		vk.descpool->availsets = vk.descpool->totalsets;
	}
	vk.descpool->availsets--;

	setinfo.descriptorPool = vk.descpool->pool;
	setinfo.descriptorSetCount = 1;
	setinfo.pSetLayouts = &layout;
	vkAllocateDescriptorSets(vk.device, &setinfo, &ret);

	return ret;
}

//creates a new dynamic buffer for us to use while streaming. because spoons.
static struct dynbuffer *VKBE_AllocNewBuffer(struct dynbuffer **link, enum dynbuf_e type)
{
	VkBufferUsageFlags ufl[] = {VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
	VkBufferCreateInfo bufinf = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	VkMemoryRequirements mem_reqs;
	VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	struct dynbuffer *n = Z_Malloc(sizeof(*n));

	bufinf.flags = 0;
	bufinf.size = n->size = (1u<<20);
	bufinf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bufinf.queueFamilyIndexCount = 0;
	bufinf.pQueueFamilyIndices = NULL;

	if (vk_usedynamicstaging & (1u<<type))
	{
		bufinf.usage = ufl[type]|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &n->devicebuf);
		bufinf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &n->stagingbuf);

		vkGetBufferMemoryRequirements(vk.device, n->devicebuf, &mem_reqs);
		n->align = mem_reqs.alignment-1;
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (memAllocInfo.memoryTypeIndex == ~0)
			memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);	//device will still be okay with this usage...
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &n->devicememory));
		VkAssert(vkBindBufferMemory(vk.device, n->devicebuf, n->devicememory, 0));

		n->renderbuf = n->devicebuf;
	}
	else
	{
		bufinf.usage = ufl[type];
		vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &n->stagingbuf);

		n->renderbuf = n->stagingbuf;
	}

	vkGetBufferMemoryRequirements(vk.device, n->stagingbuf, &mem_reqs);
	n->align = mem_reqs.alignment-1;
	memAllocInfo.allocationSize = mem_reqs.size;
	memAllocInfo.memoryTypeIndex = vk_find_memory_try(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memAllocInfo.memoryTypeIndex == ~0)
		memAllocInfo.memoryTypeIndex = vk_find_memory_try(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
//	if (memAllocInfo.memoryTypeIndex == ~0)
//		memAllocInfo.memoryTypeIndex = vk_find_memory_try(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memAllocInfo.memoryTypeIndex == ~0)
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	if (memAllocInfo.memoryTypeIndex == ~0)
		Sys_Error("Unable to allocate buffer memory");
	VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &n->stagingmemory));
	VkAssert(vkBindBufferMemory(vk.device, n->stagingbuf, n->stagingmemory, 0));

	VkAssert(vkMapMemory(vk.device, n->stagingmemory, 0, n->size, 0, &n->ptr));	//persistent-mapped.

	n->next = *link;
	*link = n;
	return n;
}
static void *fte_restrict VKBE_AllocateBufferSpace(enum dynbuf_e type, size_t datasize, VkBuffer *buf, VkDeviceSize *offset)
{	//FIXME: ubos need alignment
	struct dynbuffer *b = vk.dynbuf[type];
	void *ret;
	if (b->offset + datasize > b->size)
	{
		//flush the old one, just in case.
		VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.offset = b->flushed;
		range.size = b->offset-b->flushed;
		range.memory = b->stagingmemory;
		vkFlushMappedMemoryRanges(vk.device, 1, &range);

		if (b->devicebuf != VK_NULL_HANDLE)
		{
			struct vk_fencework *fence = VK_FencedBegin(NULL, 0);
			VkBufferCopy bcr = {0};
			bcr.srcOffset = b->flushed;
			bcr.dstOffset = b->flushed;
			bcr.size = b->offset-b->flushed;
			vkCmdCopyBuffer(fence->cbuf, b->stagingbuf, b->devicebuf, 1, &bcr);
			VK_FencedSubmit(fence);
		}

		if (!b->next)
			VKBE_AllocNewBuffer(&b->next, type);
		b = vk.dynbuf[type] = b->next;
		b->offset = 0;
		b->flushed = 0;
	}

	*buf = b->renderbuf;
	*offset = b->offset;

	ret = (qbyte*)b->ptr + b->offset;
	b->offset += datasize;
	return ret;
}

//called when a new swapchain has been created.
//makes sure there's no nulls or anything.
void VKBE_InitFramePools(struct vkframe *frame)
{
	uint32_t i;
	for (i = 0; i < DB_MAX; i++)
	{
		frame->dynbufs[i] = NULL;
		VKBE_AllocNewBuffer(&frame->dynbufs[i], i);
	}
	frame->descpools = VKBE_CreateDescriptorPool();


	{
		VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cbai.commandPool = vk.cmdpool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;
		VkAssert(vkAllocateCommandBuffers(vk.device, &cbai, &frame->cbuf));
	}

	{
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VkAssert(vkCreateFence(vk.device,&fci,vkallocationcb,&frame->finishedfence));
	}
}

//called just before submits
//makes sure that our persistent-mapped memory writes can actually be seen by the hardware.
void VKBE_FlushDynamicBuffers(void)
{
	struct vk_fencework *fence = NULL;
	uint32_t i;
	struct dynbuffer *d;
	VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};

	for (i = 0; i < DB_MAX; i++)
	{
		d = vk.dynbuf[i];
		if (d->flushed == d->offset)
			continue;

		range.offset = d->flushed;
		range.size = d->offset - d->flushed;
		range.memory = d->stagingmemory;
		vkFlushMappedMemoryRanges(vk.device, 1, &range);

		if (d->devicebuf != VK_NULL_HANDLE)
		{	
			VkBufferCopy bcr = {0};
			bcr.srcOffset = d->flushed;
			bcr.dstOffset = d->flushed;
			bcr.size = d->offset - d->flushed;
			if (!fence)
				fence = VK_FencedBegin(NULL, 0);
			vkCmdCopyBuffer(fence->cbuf, d->stagingbuf, d->devicebuf, 1, &bcr);
		}
		d->flushed = d->offset;
	}

	if (fence)
		VK_FencedSubmit(fence);
}

void VKBE_Set2D(qboolean twodee)
{
	if (twodee)
		shaderstate.forcebeflags = BEF_FORCENODEPTH;
	else
		shaderstate.forcebeflags = 0;
	shaderstate.curtime = realtime;
}

//called at the start of each frame
//resets the working dynamic buffers to this frame's storage, to avoid stepping on frames owned by the gpu
void VKBE_RestartFrame(void)
{
	uint32_t i;
	for (i = 0; i < DB_MAX; i++)
	{
		vk.dynbuf[i] = vk.frame->dynbufs[i];
		vk.dynbuf[i]->offset = vk.dynbuf[i]->flushed = 0;
	}

	shaderstate.activepipeline = VK_NULL_HANDLE;
	vk.descpool = vk.frame->descpools;
	vkResetDescriptorPool(vk.device, vk.descpool->pool, 0);
	vk.descpool->availsets = vk.descpool->totalsets;
}

void VKBE_ShutdownFramePools(struct vkframe *frame)
{
	struct dynbuffer *db;
	struct descpool *dp;
	uint32_t i;

	for (i = 0; i < DB_MAX; i++)
	{
		while(frame->dynbufs[i])
		{
			db = frame->dynbufs[i];
			vkDestroyBuffer(vk.device, db->stagingbuf, vkallocationcb);
			vkFreeMemory(vk.device, db->stagingmemory, vkallocationcb);
			if (db->devicebuf != VK_NULL_HANDLE)
			{
				vkDestroyBuffer(vk.device, db->devicebuf, vkallocationcb);
				vkFreeMemory(vk.device, db->devicememory, vkallocationcb);
			}
			frame->dynbufs[i] = db->next;
			Z_Free(db);
		}
	}

	while(frame->descpools)
	{
		dp = frame->descpools;
		vkDestroyDescriptorPool(vk.device, dp->pool, vkallocationcb);
		frame->descpools = dp->next;
		Z_Free(dp);
	}
}

void VKBE_Shutdown(void)
{
	if (!shaderstate.inited)
		return;

#ifdef RTLIGHTS
	Sh_Shutdown();
#endif

	Shader_ReleaseGeneric(shaderstate.programfixedemu[0]);
	Shader_ReleaseGeneric(shaderstate.programfixedemu[1]);

	shaderstate.inited = false;
#ifdef RTLIGHTS
	VK_TerminateShadowMap();
#endif
	Z_Free(shaderstate.wbatches);
	shaderstate.wbatches = NULL;

	vkDestroyBuffer(vk.device, shaderstate.staticbuf, vkallocationcb);
	vkFreeMemory(vk.device, shaderstate.staticbufmem, vkallocationcb);
}

static texid_t SelectPassTexture(const shaderpass_t *pass)
{
	switch(pass->texgen)
	{
	default:

	case T_GEN_DIFFUSE:
		return shaderstate.curtexnums->base;
	case T_GEN_NORMALMAP:
		if (TEXLOADED(shaderstate.curtexnums->bump))
			return shaderstate.curtexnums->bump;
		else
			return missing_texture_normal;
	case T_GEN_SPECULAR:
		if (TEXLOADED(shaderstate.curtexnums->specular))
			return shaderstate.curtexnums->specular;
		else
			return missing_texture_gloss;
	case T_GEN_UPPEROVERLAY:
		return shaderstate.curtexnums->upperoverlay;
	case T_GEN_LOWEROVERLAY:
		return shaderstate.curtexnums->loweroverlay;
	case T_GEN_FULLBRIGHT:
		return shaderstate.curtexnums->fullbright;
	case T_GEN_ANIMMAP:
		return pass->anim_frames[(int)(pass->anim_fps * shaderstate.curtime) % pass->anim_numframes];
	case T_GEN_3DMAP:
	case T_GEN_CUBEMAP:
	case T_GEN_SINGLEMAP:
		return pass->anim_frames[0];
	case T_GEN_DELUXMAP:
		{
			int lmi = shaderstate.curbatch->lightmap[0];
			if (lmi < 0 || !lightmap[lmi]->hasdeluxe)
				return r_nulltex;
			else
			{
				lmi+=1;
				return lightmap[lmi]->lightmap_texture;
			}
		}
	case T_GEN_LIGHTMAP:
		{
			int lmi = shaderstate.curbatch->lightmap[0];
			if (lmi < 0)
				return r_whiteimage;
			else
				return lightmap[lmi]->lightmap_texture;
		}

	case T_GEN_CURRENTRENDER:
		return shaderstate.tex_currentrender;
	case T_GEN_VIDEOMAP:
#ifdef HAVE_MEDIA_DECODER
		if (pass->cin)
			return Media_UpdateForShader(pass->cin);
#endif
		return r_nulltex;

	case T_GEN_LIGHTCUBEMAP:	//light's projected cubemap
		if (shaderstate.curdlight)
			return shaderstate.curdlight->cubetexture;
		else
			return r_nulltex;

	case T_GEN_SHADOWMAP:	//light's depth values.
		return shaderstate.currentshadowmap;

	case T_GEN_REFLECTION:	//reflection image (mirror-as-fbo)
		return &shaderstate.rt_reflection.q_colour;
	case T_GEN_REFRACTION:	//refraction image (portal-as-fbo)
		return shaderstate.tex_refraction;
	case T_GEN_REFRACTIONDEPTH:	//refraction image (portal-as-fbo)
		return &shaderstate.rt_refraction.q_depth;
	case T_GEN_RIPPLEMAP:	//ripplemap image (water surface distortions-as-fbo)
		return shaderstate.tex_ripplemap;

	case T_GEN_SOURCECOLOUR: //used for render-to-texture targets
		return vk.sourcecolour;
	case T_GEN_SOURCEDEPTH:	//used for render-to-texture targets
		return vk.sourcedepth;

	case T_GEN_SOURCECUBE:	//used for render-to-texture targets
		return r_nulltex;
	}
}

static void T_Gen_CurrentRender(void)
{
	vk_image_t *img;
	/*gah... I pitty the gl drivers*/
	if (!shaderstate.tex_currentrender)
	{
		shaderstate.tex_currentrender = Image_CreateTexture("***$currentrender***", NULL, 0);
		shaderstate.tex_currentrender->vkimage = Z_Malloc(sizeof(*shaderstate.tex_currentrender->vkimage));
	}
	img = shaderstate.tex_currentrender->vkimage;
	if (img->width != vid.fbpwidth || img->height != vid.fbpheight)
	{
		//FIXME: free the old image when its safe to do so.
		*img = VK_CreateTexture2DArray(vid.fbpwidth, vid.fbpheight, 1, 1, PTI_BGRA8, PTI_2D);

		if (!img->sampler)
			VK_CreateSampler(shaderstate.tex_currentrender->flags, img);
	}


	vkCmdEndRenderPass(vk.frame->cbuf);
	
	//submit now?

	//copy the backbuffer to our image
	{
		VkImageCopy region;
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount = 1;
		region.srcOffset.x = 0;
		region.srcOffset.y = 0;
		region.srcOffset.z = 0;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount = 1;
		region.dstOffset.x = 0;
		region.dstOffset.y = 0;
		region.dstOffset.z = 0;
		region.extent.width = vid.fbpwidth;
		region.extent.height = vid.fbpheight;
		region.extent.depth = 1;

		set_image_layout(vk.frame->cbuf, vk.frame->backbuf->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
		set_image_layout(vk.frame->cbuf, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
		vkCmdCopyImage(vk.frame->cbuf, vk.frame->backbuf->colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		set_image_layout(vk.frame->cbuf, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
		set_image_layout(vk.frame->cbuf, vk.frame->backbuf->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	}


	//submit now?
	//barrier?
	vkCmdBeginRenderPass(vk.frame->cbuf, &vk.rendertarg->restartinfo, VK_SUBPASS_CONTENTS_INLINE);
	//fixme: viewport+scissor?
}

static void R_FetchPlayerColour(unsigned int cv, vec3_t rgb)
{
	int i;

	if (cv >= 16)
	{
		rgb[0] = (((cv&0xff0000)>>16)**((unsigned char*)&d_8to24rgbtable[15]+0)) / (256.0*256);
		rgb[1] = (((cv&0x00ff00)>>8)**((unsigned char*)&d_8to24rgbtable[15]+1)) / (256.0*256);
		rgb[2] = (((cv&0x0000ff)>>0)**((unsigned char*)&d_8to24rgbtable[15]+2)) / (256.0*256);
		return;
	}
	i = cv;
	if (i >= 8)
	{
		i<<=4;
	}
	else
	{
		i<<=4;
		i+=15;
	}
	i*=3;
	rgb[0] = host_basepal[i+0] / 255.0;
	rgb[1] = host_basepal[i+1] / 255.0;
	rgb[2] = host_basepal[i+2] / 255.0;
/*	if (!gammaworks)
	{
		*retred = gammatable[*retred];
		*retgreen = gammatable[*retgreen];
		*retblue = gammatable[*retblue];
	}*/
}

//source is always packed
//dest is packed too
static void colourgen(const shaderpass_t *pass, int cnt, byte_vec4_t *srcb, avec4_t *srcf, vec4_t *dst, const mesh_t *mesh)
{
	switch (pass->rgbgen)
	{
	case RGB_GEN_ENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.curentity->shaderRGBAf[0];
			dst[cnt][1] = shaderstate.curentity->shaderRGBAf[1];
			dst[cnt][2] = shaderstate.curentity->shaderRGBAf[2];
		}
		break;
	case RGB_GEN_ONE_MINUS_ENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = 1-shaderstate.curentity->shaderRGBAf[0];
			dst[cnt][1] = 1-shaderstate.curentity->shaderRGBAf[1];
			dst[cnt][2] = 1-shaderstate.curentity->shaderRGBAf[2];
		}
		break;
	case RGB_GEN_VERTEX_LIGHTING:
#if MAXRLIGHTMAPS > 1
		if (mesh->colors4f_array[1])
		{
			float lm[MAXRLIGHTMAPS];
			lm[0] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f*shaderstate.identitylighting;
			lm[1] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[1]]/256.0f*shaderstate.identitylighting;
			lm[2] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[2]]/256.0f*shaderstate.identitylighting;
			lm[3] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[3]]/256.0f*shaderstate.identitylighting;
			while((cnt)--)
			{
				VectorScale(		mesh->colors4f_array[0][cnt], lm[0], dst[cnt]);
				VectorMA(dst[cnt],	lm[1], mesh->colors4f_array[1][cnt], dst[cnt]);
				VectorMA(dst[cnt],	lm[2], mesh->colors4f_array[2][cnt], dst[cnt]);
				VectorMA(dst[cnt],	lm[3], mesh->colors4f_array[3][cnt], dst[cnt]);
			}
			break;
		}
#endif

		if (shaderstate.identitylighting != 1)
		{
			if (srcf)
			{
				while((cnt)--)
				{
					dst[cnt][0] = srcf[cnt][0]*shaderstate.identitylighting;
					dst[cnt][1] = srcf[cnt][1]*shaderstate.identitylighting;
					dst[cnt][2] = srcf[cnt][2]*shaderstate.identitylighting;
				}
			}
			else if (srcb)
			{
				float t = shaderstate.identitylighting * (1/255.0);
				while((cnt)--)
				{
					dst[cnt][0] = srcb[cnt][0]*t;
					dst[cnt][1] = srcb[cnt][1]*t;
					dst[cnt][2] = srcb[cnt][2]*t;
				}
			}
			else
			{
				while((cnt)--)
				{
					dst[cnt][0] = shaderstate.identitylighting;
					dst[cnt][1] = shaderstate.identitylighting;
					dst[cnt][2] = shaderstate.identitylighting;
				}
			}
			break;
		}
	case RGB_GEN_VERTEX_EXACT:
		if (srcf)
		{
			while((cnt)--)
			{
				dst[cnt][0] = srcf[cnt][0];
				dst[cnt][1] = srcf[cnt][1];
				dst[cnt][2] = srcf[cnt][2];
			}
		}
		else if (srcb)
		{
			float t = 1/255.0;
			while((cnt)--)
			{
				dst[cnt][0] = srcb[cnt][0]*t;
				dst[cnt][1] = srcb[cnt][1]*t;
				dst[cnt][2] = srcb[cnt][2]*t;
			}
		}
		else
		{
			while((cnt)--)
			{
				dst[cnt][0] = 1;
				dst[cnt][1] = 1;
				dst[cnt][2] = 1;
			}
			break;
		}
		break;
	case RGB_GEN_ONE_MINUS_VERTEX:
		if (srcf)
		{
			while((cnt)--)
			{
				dst[cnt][0] = 1-srcf[cnt][0];
				dst[cnt][1] = 1-srcf[cnt][1];
				dst[cnt][2] = 1-srcf[cnt][2];
			}
		}
		break;
	case RGB_GEN_IDENTITY_LIGHTING:
		if (shaderstate.curbatch->vtlightstyle[0] != 255 && d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]] != 256)
		{
			vec_t val = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f;
			while((cnt)--)
			{
				dst[cnt][0] = val;
				dst[cnt][1] = val;
				dst[cnt][2] = val;
			}
		}
		else
		{
			//compensate for overbrights
			while((cnt)--)
			{
				dst[cnt][0] = shaderstate.identitylighting;
				dst[cnt][1] = shaderstate.identitylighting;
				dst[cnt][2] = shaderstate.identitylighting;
			}
		}
		break;
	case RGB_GEN_IDENTITY_OVERBRIGHT:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.identitylightmap;
			dst[cnt][1] = shaderstate.identitylightmap;
			dst[cnt][2] = shaderstate.identitylightmap;
		}
		break;
	default:
	case RGB_GEN_IDENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.identitylighting;
			dst[cnt][1] = shaderstate.identitylighting;
			dst[cnt][2] = shaderstate.identitylighting;
		}
		break;
	case RGB_GEN_CONST:
		while((cnt)--)
		{
			dst[cnt][0] = pass->rgbgen_func.args[0];
			dst[cnt][1] = pass->rgbgen_func.args[1];
			dst[cnt][2] = pass->rgbgen_func.args[2];
		}
		break;
	case RGB_GEN_LIGHTING_DIFFUSE:
		//collect lighting details for mobile entities
		if (!mesh->normals_array)
		{
			while((cnt)--)
			{
				dst[cnt][0] = 1;
				dst[cnt][1] = 1;
				dst[cnt][2] = 1;
			}
		}
		else
		{
			R_LightArrays(shaderstate.curentity, mesh->xyz_array, dst, cnt, mesh->normals_array, shaderstate.identitylighting);
		}
		break;
	case RGB_GEN_WAVE:
		{
			float *table;
			float c;

			table = FTableForFunc(pass->rgbgen_func.type);
			c = pass->rgbgen_func.args[2] + shaderstate.curtime * pass->rgbgen_func.args[3];
			c = FTABLE_EVALUATE(table, c) * pass->rgbgen_func.args[1] + pass->rgbgen_func.args[0];
			c = bound(0.0f, c, 1.0f);

			while((cnt)--)
			{
				dst[cnt][0] = c;
				dst[cnt][1] = c;
				dst[cnt][2] = c;
			}
		}
		break;

	case RGB_GEN_TOPCOLOR:
		if (cnt)
		{
			vec3_t rgb;
			R_FetchPlayerColour(shaderstate.curentity->topcolour, rgb);
			while((cnt)--)
			{
				dst[cnt][0] = rgb[0];
				dst[cnt][1] = rgb[1];
				dst[cnt][2] = rgb[2];
			}
		}
		break;
	case RGB_GEN_BOTTOMCOLOR:
		if (cnt)
		{
			vec3_t rgb;
			R_FetchPlayerColour(shaderstate.curentity->bottomcolour, rgb);
			while((cnt)--)
			{
				dst[cnt][0] = rgb[0];
				dst[cnt][1] = rgb[1];
				dst[cnt][2] = rgb[2];
			}
		}
		break;
	}
}
static void alphagen(const shaderpass_t *pass, int cnt, byte_vec4_t *srcb, avec4_t *srcf, avec4_t *dst, const mesh_t *mesh)
{
	float *table;
	float t;
	float f;
	vec3_t v1, v2;
	int i;

	switch (pass->alphagen)
	{
	default:
	case ALPHA_GEN_IDENTITY:
		if (shaderstate.flags & BEF_FORCETRANSPARENT)
		{
			while(cnt--)
				dst[cnt][3] = shaderstate.curentity->shaderRGBAf[3];
		}
		else
		{
			while(cnt--)
				dst[cnt][3] = 1;
		}
		break;

	case ALPHA_GEN_CONST:
		t = pass->alphagen_func.args[0];
		while(cnt--)
			dst[cnt][3] = t;
		break;

	case ALPHA_GEN_WAVE:
		table = FTableForFunc(pass->alphagen_func.type);
		f = pass->alphagen_func.args[2] + shaderstate.curtime * pass->alphagen_func.args[3];
		f = FTABLE_EVALUATE(table, f) * pass->alphagen_func.args[1] + pass->alphagen_func.args[0];
		t = bound(0.0f, f, 1.0f);
		while(cnt--)
			dst[cnt][3] = t;
		break;

	case ALPHA_GEN_PORTAL:
		//FIXME: should this be per-vert?
		if (r_refdef.recurse)
			f = 1;
		else
		{
			VectorAdd(mesh->xyz_array[0], shaderstate.curentity->origin, v1);
			VectorSubtract(r_origin, v1, v2);
			f = VectorLength(v2) * (1.0 / shaderstate.curshader->portaldist);
			f = bound(0.0f, f, 1.0f);
		}

		while(cnt--)
			dst[cnt][3] = f;
		break;

	case ALPHA_GEN_VERTEX:
		if (srcf)
		{
			while(cnt--)
			{
				dst[cnt][3] = srcf[cnt][3];
			}
		}
		else if (srcb)
		{
			float t = 1/255.0;
			while(cnt--)
			{
				dst[cnt][3] = srcb[cnt][3]*t;
			}
		}
		else
		{
			while(cnt--)
			{
				dst[cnt][3] = 1;
			}
			break;
		}
		break;

	case ALPHA_GEN_ENTITY:
		f = bound(0, shaderstate.curentity->shaderRGBAf[3], 1);
		while(cnt--)
		{
			dst[cnt][3] = f;
		}
		break;


	case ALPHA_GEN_SPECULAR:
		{
			VectorSubtract(r_origin, shaderstate.curentity->origin, v1);

			if (!Matrix3_Compare((const vec3_t*)shaderstate.curentity->axis, (const vec3_t*)axisDefault))
			{
				Matrix3_Multiply_Vec3(shaderstate.curentity->axis, v1, v2);
			}
			else
			{
				VectorCopy(v1, v2);
			}

			for (i = 0; i < cnt; i++)
			{
				VectorSubtract(v2, mesh->xyz_array[i], v1);
				f = DotProduct(v1, mesh->normals_array[i] ) * Q_rsqrt(DotProduct(v1,v1));
				f = f * f * f * f * f;
				dst[i][3] = bound (0.0f, f, 1.0f);
			}
		}
		break;
	}
}

//true if we used an array (flag to use uniforms for it instead if false)
static void BE_GenerateColourMods(unsigned int vertcount, const shaderpass_t *pass, VkBuffer *buffer, VkDeviceSize *offset)
{
	const mesh_t *m = shaderstate.meshlist[0];
//	if (pass->flags & SHADER_PASS_NOCOLORARRAY)
//		error
	if (					   ((pass->rgbgen == RGB_GEN_VERTEX_LIGHTING) ||
								(pass->rgbgen == RGB_GEN_VERTEX_EXACT) ||
								(pass->rgbgen == RGB_GEN_ONE_MINUS_VERTEX)) &&
								(pass->alphagen == ALPHA_GEN_VERTEX))
	{
		if (shaderstate.batchvbo)
		{	//just use the colour vbo provided
			*buffer = shaderstate.batchvbo->colours[0].vk.buff;
			*offset = shaderstate.batchvbo->colours[0].vk.offs;
		}
		else
		{	//we can't use the vbo due to gaps that we don't want to have to deal with
			//we can at least ensure that the data is written in one go to aid cpu cache.
			vec4_t *map;
			unsigned int mno;
			map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec4_t), buffer, offset);
			if (m->colors4f_array[0])
			{
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->colors4f_array[0], m->numvertexes * sizeof(vec4_t));
					map += m->numvertexes;
				}
			}
			else if (m->colors4b_array)
			{
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					uint32_t v;
					m = shaderstate.meshlist[mno];
					for (v = 0; v < m->numvertexes; v++)
						Vector4Scale(m->colors4b_array[v], 1.0/255, map[v]);
					map += m->numvertexes;
				}
			}
			else
			{
				for (mno = 0; mno < vertcount; mno++)
					Vector4Set(map[mno], 1, 1, 1, 1);
			}
		}
	}
	else
	{
		vec4_t *map;
		unsigned int mno;
		map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec4_t), buffer, offset);
		for (mno = 0; mno < shaderstate.nummeshes; mno++)
		{
			m = shaderstate.meshlist[mno];
			colourgen(pass, m->numvertexes, m->colors4b_array, m->colors4f_array[0], map, m);
			alphagen(pass, m->numvertexes, m->colors4b_array, m->colors4f_array[0], map, m);
			map += m->numvertexes;
		}
	}
}

/*********************************************************************************************************/
/*========================================== texture coord generation =====================================*/
static void tcgen_environment(float *st, unsigned int numverts, float *xyz, float *normal)
{
	int			i;
	vec3_t		viewer, reflected;
	float		d;

	vec3_t		rorg;

	RotateLightVector(shaderstate.curentity->axis, shaderstate.curentity->origin, r_origin, rorg);

	for (i = 0 ; i < numverts ; i++, xyz += sizeof(vecV_t)/sizeof(vec_t), normal += 3, st += 2 )
	{
		VectorSubtract (rorg, xyz, viewer);
		VectorNormalizeFast (viewer);

		d = DotProduct (normal, viewer);

		reflected[0] = normal[0]*2*d - viewer[0];
		reflected[1] = normal[1]*2*d - viewer[1];
		reflected[2] = normal[2]*2*d - viewer[2];

		st[0] = 0.5 + reflected[1] * 0.5;
		st[1] = 0.5 - reflected[2] * 0.5;
	}
}

static float *tcgen(const shaderpass_t *pass, int cnt, float *dst, const mesh_t *mesh)
{
	int i;
	vecV_t *src;
	switch (pass->tcgen)
	{
	default:
	case TC_GEN_BASE:
		return (float*)mesh->st_array;
	case TC_GEN_LIGHTMAP:
		return (float*)mesh->lmst_array[0];
	case TC_GEN_NORMAL:
		return (float*)mesh->normals_array;
	case TC_GEN_SVECTOR:
		return (float*)mesh->snormals_array;
	case TC_GEN_TVECTOR:
		return (float*)mesh->tnormals_array;
	case TC_GEN_ENVIRONMENT:
		if (!mesh->normals_array)
			return (float*)mesh->st_array;
		tcgen_environment(dst, cnt, (float*)mesh->xyz_array, (float*)mesh->normals_array);
		return dst;

	case TC_GEN_DOTPRODUCT:
		return dst;//mesh->st_array[0];
	case TC_GEN_VECTOR:
		src = mesh->xyz_array;
		for (i = 0; i < cnt; i++, dst += 2)
		{
			dst[0] = DotProduct(pass->tcgenvec[0], src[i]);
			dst[1] = DotProduct(pass->tcgenvec[1], src[i]);
		}
		return dst;
	}
}

/*src and dst can be the same address when tcmods are chained*/
static void tcmod(const tcmod_t *tcmod, int cnt, const float *src, float *dst, const mesh_t *mesh)
{
	float *table;
	float t1, t2;
	float cost, sint;
	int j;

	switch (tcmod->type)
	{
		case SHADER_TCMOD_ROTATE:
			cost = tcmod->args[0] * shaderstate.curtime;
			sint = R_FastSin(cost);
			cost = R_FastSin(cost + 0.25);

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				t1 = cost * (src[0] - 0.5f) - sint * (src[1] - 0.5f) + 0.5f;
				t2 = cost * (src[1] - 0.5f) + sint * (src[0] - 0.5f) + 0.5f;
				dst[0] = t1;
				dst[1] = t2;
			}
			break;

		case SHADER_TCMOD_SCALE:
			t1 = tcmod->args[0];
			t2 = tcmod->args[1];

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] * t1;
				dst[1] = src[1] * t2;
			}
			break;

		case SHADER_TCMOD_TURB:
			t1 = tcmod->args[2] + shaderstate.curtime * tcmod->args[3];
			t2 = tcmod->args[1];

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] + R_FastSin (src[0]*t2+t1) * t2;
				dst[1] = src[1] + R_FastSin (src[1]*t2+t1) * t2;
			}
			break;

		case SHADER_TCMOD_STRETCH:
			table = FTableForFunc(tcmod->args[0]);
			t2 = tcmod->args[3] + shaderstate.curtime * tcmod->args[4];
			t1 = FTABLE_EVALUATE(table, t2) * tcmod->args[2] + tcmod->args[1];
			t1 = t1 ? 1.0f / t1 : 1.0f;
			t2 = 0.5f - 0.5f * t1;
			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] * t1 + t2;
				dst[1] = src[1] * t1 + t2;
			}
			break;

		case SHADER_TCMOD_SCROLL:
			t1 = tcmod->args[0] * shaderstate.curtime;
			t2 = tcmod->args[1] * shaderstate.curtime;

			for (j = 0; j < cnt; j++, dst += 2, src+=2)
			{
				dst[0] = src[0] + t1;
				dst[1] = src[1] + t2;
			}
			break;

		case SHADER_TCMOD_TRANSFORM:
			for (j = 0; j < cnt; j++, dst+=2, src+=2)
			{
				t1 = src[0];
				t2 = src[1];
				dst[0] = t1 * tcmod->args[0] + t2 * tcmod->args[2] + tcmod->args[4];
				dst[1] = t1 * tcmod->args[1] + t1 * tcmod->args[3] + tcmod->args[5];
			}
			break;

		default:
			break;
	}
}

static void BE_GenerateTCMods(const shaderpass_t *pass, float *dest)
{
	mesh_t *mesh;
	unsigned int mno;
	int i;
	float *src;
	for (mno = 0; mno < shaderstate.nummeshes; mno++)
	{
		mesh = shaderstate.meshlist[mno];
		src = tcgen(pass, mesh->numvertexes, dest, mesh);
		//tcgen might return unmodified info
		if (pass->numtcmods)
		{
			tcmod(&pass->tcmods[0], mesh->numvertexes, src, dest, mesh);
			for (i = 1; i < pass->numtcmods; i++)
			{
				tcmod(&pass->tcmods[i], mesh->numvertexes, dest, dest, mesh);
			}
		}
		else if (src != dest)
		{
			memcpy(dest, src, sizeof(vec2_t)*mesh->numvertexes);
		}
		dest += mesh->numvertexes*2;
	}
}

//end texture coords
/*******************************************************************************************************************/
static void deformgen(const deformv_t *deformv, int cnt, vecV_t *src, vecV_t *dst, const mesh_t *mesh)
{
	float *table;
	int j, k;
	float args[4];
	float deflect;
	switch (deformv->type)
	{
	default:
	case DEFORMV_NONE:
		if (src != dst)
			memcpy(dst, src, sizeof(*src)*cnt);
		break;

	case DEFORMV_WAVE:
		if (!mesh->normals_array)
		{
			if (src != dst)
				memcpy(dst, src, sizeof(*src)*cnt);
			return;
		}
		args[0] = deformv->func.args[0];
		args[1] = deformv->func.args[1];
		args[3] = deformv->func.args[2] + deformv->func.args[3] * shaderstate.curtime;
		table = FTableForFunc(deformv->func.type);

		for ( j = 0; j < cnt; j++ )
		{
			deflect = deformv->args[0] * (src[j][0]+src[j][1]+src[j][2]) + args[3];
			deflect = FTABLE_EVALUATE(table, deflect) * args[1] + args[0];

			// Deflect vertex along its normal by wave amount
			VectorMA(src[j], deflect, mesh->normals_array[j], dst[j]);
		}
		break;

	case DEFORMV_NORMAL:
		//normal does not actually move the verts, but it does change the normals array
		//we don't currently support that.
		if (src != dst)
			memcpy(dst, src, sizeof(*src)*cnt);
/*
		args[0] = deformv->args[1] * shaderstate.curtime;

		for ( j = 0; j < cnt; j++ )
		{
			args[1] = normalsArray[j][2] * args[0];

			deflect = deformv->args[0] * R_FastSin(args[1]);
			normalsArray[j][0] *= deflect;
			deflect = deformv->args[0] * R_FastSin(args[1] + 0.25);
			normalsArray[j][1] *= deflect;
			VectorNormalizeFast(normalsArray[j]);
		}
*/		break;

	case DEFORMV_MOVE:
		table = FTableForFunc(deformv->func.type);
		deflect = deformv->func.args[2] + shaderstate.curtime * deformv->func.args[3];
		deflect = FTABLE_EVALUATE(table, deflect) * deformv->func.args[1] + deformv->func.args[0];

		for ( j = 0; j < cnt; j++ )
			VectorMA(src[j], deflect, deformv->args, dst[j]);
		break;

	case DEFORMV_BULGE:
		args[0] = deformv->args[0]/(2*M_PI);
		args[1] = deformv->args[1];
		args[2] = shaderstate.curtime * deformv->args[2]/(2*M_PI);

		for (j = 0; j < cnt; j++)
		{
			deflect = R_FastSin(mesh->st_array[j][0]*args[0] + args[2])*args[1];
			dst[j][0] = src[j][0]+deflect*mesh->normals_array[j][0];
			dst[j][1] = src[j][1]+deflect*mesh->normals_array[j][1];
			dst[j][2] = src[j][2]+deflect*mesh->normals_array[j][2];
		}
		break;

	case DEFORMV_AUTOSPRITE:
		if (mesh->numindexes < 6)
			break;

		for (j = 0; j < cnt-3; j+=4, src+=4, dst+=4)
		{
			vec3_t mid, d;
			float radius;
			mid[0] = 0.25*(src[0][0] + src[1][0] + src[2][0] + src[3][0]);
			mid[1] = 0.25*(src[0][1] + src[1][1] + src[2][1] + src[3][1]);
			mid[2] = 0.25*(src[0][2] + src[1][2] + src[2][2] + src[3][2]);
			VectorSubtract(src[0], mid, d);
			radius = 2*VectorLength(d);

			for (k = 0; k < 4; k++)
			{
				dst[k][0] = mid[0] + radius*((mesh->st_array[k][0]-0.5)*r_refdef.m_view[0+0]-(mesh->st_array[k][1]-0.5)*r_refdef.m_view[0+1]);
				dst[k][1] = mid[1] + radius*((mesh->st_array[k][0]-0.5)*r_refdef.m_view[4+0]-(mesh->st_array[k][1]-0.5)*r_refdef.m_view[4+1]);
				dst[k][2] = mid[2] + radius*((mesh->st_array[k][0]-0.5)*r_refdef.m_view[8+0]-(mesh->st_array[k][1]-0.5)*r_refdef.m_view[8+1]);
			}
		}
		break;

	case DEFORMV_AUTOSPRITE2:
		if (mesh->numindexes < 6)
			break;

		for (k = 0; k < mesh->numindexes; k += 6)
		{
			int long_axis, short_axis;
			vec3_t axis;
			float len[3];
			mat3_t m0, m1, m2, result;
			float *quad[4];
			vec3_t rot_centre, tv;

			quad[0] = (float *)(dst + mesh->indexes[k+0]);
			quad[1] = (float *)(dst + mesh->indexes[k+1]);
			quad[2] = (float *)(dst + mesh->indexes[k+2]);

			for (j = 2; j >= 0; j--)
			{
				quad[3] = (float *)(dst + mesh->indexes[k+3+j]);
				if (!VectorEquals (quad[3], quad[0]) &&
					!VectorEquals (quad[3], quad[1]) &&
					!VectorEquals (quad[3], quad[2]))
				{
					break;
				}
			}

			// build a matrix were the longest axis of the billboard is the Y-Axis
			VectorSubtract(quad[1], quad[0], m0[0]);
			VectorSubtract(quad[2], quad[0], m0[1]);
			VectorSubtract(quad[2], quad[1], m0[2]);
			len[0] = DotProduct(m0[0], m0[0]);
			len[1] = DotProduct(m0[1], m0[1]);
			len[2] = DotProduct(m0[2], m0[2]);

			if ((len[2] > len[1]) && (len[2] > len[0]))
			{
				if (len[1] > len[0])
				{
					long_axis = 1;
					short_axis = 0;
				}
				else
				{
					long_axis = 0;
					short_axis = 1;
				}
			}
			else if ((len[1] > len[2]) && (len[1] > len[0]))
			{
				if (len[2] > len[0])
				{
					long_axis = 2;
					short_axis = 0;
				}
				else
				{
					long_axis = 0;
					short_axis = 2;
				}
			}
			else //if ( (len[0] > len[1]) && (len[0] > len[2]) )
			{
				if (len[2] > len[1])
				{
					long_axis = 2;
					short_axis = 1;
				}
				else
				{
					long_axis = 1;
					short_axis = 2;
				}
			}

			if (DotProduct(m0[long_axis], m0[short_axis]))
			{
				VectorNormalize2(m0[long_axis], axis);
				VectorCopy(axis, m0[1]);

				if (axis[0] || axis[1])
				{
					VectorVectors(m0[1], m0[2], m0[0]);
				}
				else
				{
					VectorVectors(m0[1], m0[0], m0[2]);
				}
			}
			else
			{
				VectorNormalize2(m0[long_axis], axis);
				VectorNormalize2(m0[short_axis], m0[0]);
				VectorCopy(axis, m0[1]);
				CrossProduct(m0[0], m0[1], m0[2]);
			}

			for (j = 0; j < 3; j++)
				rot_centre[j] = (quad[0][j] + quad[1][j] + quad[2][j] + quad[3][j]) * 0.25;

			if (shaderstate.curentity)
			{
				VectorAdd(shaderstate.curentity->origin, rot_centre, tv);
			}
			else
			{
				VectorCopy(rot_centre, tv);
			}
			VectorSubtract(r_origin, tv, tv);

			// filter any longest-axis-parts off the camera-direction
			deflect = -DotProduct(tv, axis);

			VectorMA(tv, deflect, axis, m1[2]);
			VectorNormalizeFast(m1[2]);
			VectorCopy(axis, m1[1]);
			CrossProduct(m1[1], m1[2], m1[0]);

			Matrix3_Transpose(m1, m2);
			Matrix3_Multiply(m2, m0, result);

			for (j = 0; j < 4; j++)
			{
				VectorSubtract(quad[j], rot_centre, tv);
				Matrix3_Multiply_Vec3((void *)result, tv, quad[j]);
				VectorAdd(rot_centre, quad[j], quad[j]);
			}
		}
		break;

//	case DEFORMV_PROJECTION_SHADOW:
//		break;
	}
}

static void BE_CreatePipeline(program_t *p, unsigned int shaderflags, unsigned int blendflags, unsigned int permu)
{
	struct pipeline_s *pipe;
	VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE]={0};
	VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	VkVertexInputBindingDescription vbinds[VK_BUFF_MAX] = {{0}};
	VkVertexInputAttributeDescription vattrs[VK_BUFF_MAX] = {{0}};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState att_state[1];
	VkGraphicsPipelineCreateInfo pipeCreateInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	VkPipelineShaderStageCreateInfo shaderStages[2] = {{0}};
	struct specdata_s
	{
		int alphamode;
		int permu[16];
		union
		{
			float f;
			int i;
		} cvars[64];
	} specdata;
	VkSpecializationMapEntry specentries[256] = {{0}};
	VkSpecializationInfo specInfo = {0}, *bugsbeware;
	VkResult err;
	uint32_t i, s;
	unsigned char *cvardata;

	if (!p->vert || !p->frag)
		Sys_Error("program missing required shader\n");	//PANIC


	pipe = Z_Malloc(sizeof(*pipe));
	if (!p->pipelines)
		p->pipelines = pipe;
	else
	{	//insert at end. if it took us a while to realise that we needed it, chances are its not that common.
		//so don't cause the other pipelines to waste cycles for it.
		struct pipeline_s *prev;
		for (prev = p->pipelines; ; prev = prev->next)
			if (!prev->next)
				break;
		prev->next = pipe;
	}

	pipe->flags = shaderflags;
	pipe->blendbits = blendflags;
	pipe->permu = permu;

	if (permu&PERMUTATION_BEM_WIREFRAME)
	{
		blendflags |= SBITS_MISC_NODEPTHTEST;
		blendflags &= ~SBITS_MISC_DEPTHWRITE;

		blendflags &= ~(SHADER_CULL_FRONT|SHADER_CULL_BACK);
	}

	dyn.flags = 0;
	dyn.dynamicStateCount = 0;
	dyn.pDynamicStates = dynamicStateEnables;

	//it wasn't supposed to be like this!
	//this stuff gets messy with tcmods and rgbgen/alphagen stuff
	vbinds[VK_BUFF_POS].binding = VK_BUFF_POS;
	vbinds[VK_BUFF_POS].stride = sizeof(vecV_t);
	vbinds[VK_BUFF_POS].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_POS].binding = vbinds[VK_BUFF_POS].binding;
	vattrs[VK_BUFF_POS].location = VK_BUFF_POS;
	vattrs[VK_BUFF_POS].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[VK_BUFF_POS].offset = 0;
	vbinds[VK_BUFF_TC].binding = VK_BUFF_TC;
	vbinds[VK_BUFF_TC].stride = sizeof(vec2_t);
	vbinds[VK_BUFF_TC].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_TC].binding = vbinds[VK_BUFF_TC].binding;
	vattrs[VK_BUFF_TC].location = VK_BUFF_TC;
	vattrs[VK_BUFF_TC].format = VK_FORMAT_R32G32_SFLOAT;
	vattrs[VK_BUFF_TC].offset = 0;
	vbinds[VK_BUFF_COL].binding = VK_BUFF_COL;
	vbinds[VK_BUFF_COL].stride = sizeof(vec4_t);
	vbinds[VK_BUFF_COL].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_COL].binding = vbinds[VK_BUFF_COL].binding;
	vattrs[VK_BUFF_COL].location = VK_BUFF_COL;
	vattrs[VK_BUFF_COL].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	vattrs[VK_BUFF_COL].offset = 0;
	vbinds[VK_BUFF_LMTC].binding = VK_BUFF_LMTC;
	vbinds[VK_BUFF_LMTC].stride = sizeof(vec2_t);
	vbinds[VK_BUFF_LMTC].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_LMTC].binding = vbinds[VK_BUFF_LMTC].binding;
	vattrs[VK_BUFF_LMTC].location = VK_BUFF_LMTC;
	vattrs[VK_BUFF_LMTC].format = VK_FORMAT_R32G32_SFLOAT;
	vattrs[VK_BUFF_LMTC].offset = 0;

	//fixme: in all seriousness, why is this not a single buffer?
	vbinds[VK_BUFF_NORM].binding = VK_BUFF_NORM;
	vbinds[VK_BUFF_NORM].stride = sizeof(vec3_t);
	vbinds[VK_BUFF_NORM].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_NORM].binding = vbinds[VK_BUFF_NORM].binding;
	vattrs[VK_BUFF_NORM].location = VK_BUFF_NORM;
	vattrs[VK_BUFF_NORM].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[VK_BUFF_NORM].offset = 0;
	vbinds[VK_BUFF_SDIR].binding = VK_BUFF_SDIR;
	vbinds[VK_BUFF_SDIR].stride = sizeof(vec3_t);
	vbinds[VK_BUFF_SDIR].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_SDIR].binding = vbinds[VK_BUFF_SDIR].binding;
	vattrs[VK_BUFF_SDIR].location = VK_BUFF_SDIR;
	vattrs[VK_BUFF_SDIR].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[VK_BUFF_SDIR].offset = 0;
	vbinds[VK_BUFF_TDIR].binding = VK_BUFF_TDIR;
	vbinds[VK_BUFF_TDIR].stride = sizeof(vec3_t);
	vbinds[VK_BUFF_TDIR].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	vattrs[VK_BUFF_TDIR].binding = vbinds[VK_BUFF_TDIR].binding;
	vattrs[VK_BUFF_TDIR].location = VK_BUFF_TDIR;
	vattrs[VK_BUFF_TDIR].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[VK_BUFF_TDIR].offset = 0;

	vi.vertexBindingDescriptionCount = countof(vbinds);
	vi.pVertexBindingDescriptions = vbinds;
	vi.vertexAttributeDescriptionCount = countof(vattrs);
	vi.pVertexAttributeDescriptions = vattrs;

	ia.topology = (blendflags&SBITS_LINES)?VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	vp.viewportCount = 1;
	dynamicStateEnables[dyn.dynamicStateCount++] =	VK_DYNAMIC_STATE_VIEWPORT;
	vp.scissorCount = 1;
	dynamicStateEnables[dyn.dynamicStateCount++] =	VK_DYNAMIC_STATE_SCISSOR;
	//FIXME: fillModeNonSolid might mean mode_line is not supported.
	rs.polygonMode = (permu&PERMUTATION_BEM_WIREFRAME)?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL;
	rs.lineWidth = 1;
	rs.cullMode = ((shaderflags&SHADER_CULL_FRONT)?VK_CULL_MODE_FRONT_BIT:0) | ((shaderflags&SHADER_CULL_BACK)?VK_CULL_MODE_BACK_BIT:0);
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.depthClampEnable = VK_FALSE;
	rs.rasterizerDiscardEnable = VK_FALSE;
	if (shaderflags & SHADER_POLYGONOFFSET)
	{
		rs.depthBiasEnable = VK_TRUE;
		rs.depthBiasConstantFactor = -25;//shader->polyoffset.unit;
		rs.depthBiasClamp = 0;
		rs.depthBiasSlopeFactor = -0.05;//shader->polyoffset.factor;
	}
	else
		rs.depthBiasEnable = VK_FALSE;

	ms.pSampleMask = NULL;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	ds.depthTestEnable = (blendflags&SBITS_MISC_NODEPTHTEST)?VK_FALSE:VK_TRUE;
	ds.depthWriteEnable = (blendflags&SBITS_MISC_DEPTHWRITE)?VK_TRUE:VK_FALSE;
	if (blendflags & SBITS_MISC_DEPTHEQUALONLY)
		ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
	else if (blendflags & SBITS_MISC_DEPTHCLOSERONLY)
		ds.depthCompareOp = VK_COMPARE_OP_LESS;
	else
		ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.back.failOp = VK_STENCIL_OP_KEEP;
	ds.back.passOp = VK_STENCIL_OP_KEEP;
	ds.back.compareOp = VK_COMPARE_OP_NEVER;//VK_COMPARE_OP_ALWAYS;
	ds.stencilTestEnable = VK_FALSE;
	ds.front = ds.back;
	memset(att_state, 0, sizeof(att_state));
	att_state[0].colorWriteMask =
		((blendflags&SBITS_MASK_RED)?0:VK_COLOR_COMPONENT_R_BIT) |
		((blendflags&SBITS_MASK_GREEN)?0:VK_COLOR_COMPONENT_G_BIT) |
		((blendflags&SBITS_MASK_BLUE)?0:VK_COLOR_COMPONENT_B_BIT) |
		((blendflags&SBITS_MASK_ALPHA)?0:VK_COLOR_COMPONENT_A_BIT);

	if (blendflags & SBITS_BLEND_BITS)
	{
		switch(blendflags & SBITS_SRCBLEND_BITS)
		{
		case SBITS_SRCBLEND_ZERO:					att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;				att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;				break;
		case SBITS_SRCBLEND_ONE:					att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;					att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;					break;
		case SBITS_SRCBLEND_DST_COLOR:				att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;			att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;			break;
		case SBITS_SRCBLEND_ONE_MINUS_DST_COLOR:	att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;	att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;	break;
		case SBITS_SRCBLEND_SRC_ALPHA:				att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;			att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;			break;
		case SBITS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;	att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;	break;
		case SBITS_SRCBLEND_DST_ALPHA:				att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;			att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;			break;
		case SBITS_SRCBLEND_ONE_MINUS_DST_ALPHA:	att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;	att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;	break;
		case SBITS_SRCBLEND_ALPHA_SATURATE:			att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;	att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;	break;
		default:	Sys_Error("Bad shader blend src\n"); return;
		}
		switch(blendflags & SBITS_DSTBLEND_BITS)
		{
		case SBITS_DSTBLEND_ZERO:					att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;				att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;				break;
		case SBITS_DSTBLEND_ONE:					att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;					att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;					break;
		case SBITS_DSTBLEND_SRC_ALPHA:				att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;			att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;			break;
		case SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;	att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;	break;
		case SBITS_DSTBLEND_DST_ALPHA:				att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;			att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;			break;
		case SBITS_DSTBLEND_ONE_MINUS_DST_ALPHA:	att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;	att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;	break;
		case SBITS_DSTBLEND_SRC_COLOR:				att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;			att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;			break;
		case SBITS_DSTBLEND_ONE_MINUS_SRC_COLOR:	att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;	att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;	break;
		default:	Sys_Error("Bad shader blend dst\n"); return;
		}
		att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
		att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
		att_state[0].blendEnable = VK_TRUE;
	}
	else
	{
		att_state[0].blendEnable = VK_FALSE;
	}
	if (permu&PERMUTATION_BEM_DEPTHONLY)
		cb.attachmentCount = 0;
	else
		cb.attachmentCount = 1;
	cb.pAttachments = att_state;


	s = 0;
	specentries[s].constantID = 0;
	specentries[s].offset = offsetof(struct specdata_s, alphamode);
	specentries[s].size = sizeof(specdata.alphamode);
	s++;
	if (blendflags & SBITS_ATEST_GE128)
		specdata.alphamode = 3;
	else if (blendflags & SBITS_ATEST_GT0)
		specdata.alphamode = 2;
	else if (blendflags & SBITS_ATEST_LT128)
		specdata.alphamode = 1;
	else //if (blendflags & SBITS_ATEST_NONE)
		specdata.alphamode = 0;

	for (i = 0; i < countof(specdata.permu); i++)
	{
		specentries[s].constantID = 16+i;
		specentries[s].offset = offsetof(struct specdata_s, permu[i]);
		specentries[s].size = sizeof(specdata.permu[i]);
		s++;
		specdata.permu[i] = !!(permu & (1u<<i));
	}

	//cvars
	for (cvardata = p->cvardata, i = 0; cvardata < p->cvardata + p->cvardatasize; )
	{
		unsigned short id = (cvardata[0]<<8)|cvardata[1];
		unsigned char type = cvardata[2], size = cvardata[3]-'0';
		char *name;
		cvar_t *var;
		unsigned int u;

		cvardata += 4;
		name = cvardata;
		cvardata += strlen(name)+1;

		if (i + size > countof(specdata.cvars))
			break;	//error

		if (type >= 'A' && type <= 'Z')
		{	//args will be handled by the blob loader.
			for (u = 0; u < size && u < 4; u++)
			{
				specentries[s].constantID = id;
				specentries[s].offset = offsetof(struct specdata_s, cvars[i]);
				specentries[s].size = sizeof(specdata.cvars[i]);

				specdata.cvars[i].i = (cvardata[u*4+0]<<24)|(cvardata[u*4+1]<<16)|(cvardata[u*4+2]<<8)|(cvardata[u*4+3]<<0);
				s++;
				i++;
				id++;
			}
		}
		else
		{
			var = Cvar_FindVar(name);
			if (var)
			{
				for (u = 0; u < size && u < 4; u++)
				{
					specentries[s].constantID = id;
					specentries[s].offset = offsetof(struct specdata_s, cvars[i]);
					specentries[s].size = sizeof(specdata.cvars[i]);

					if (type == 'i')
						specdata.cvars[i].i = var->ival;
					else
						specdata.cvars[i].f = var->vec4[u];
					s++;
					i++;
					id++;
				}
			}
		}
		cvardata += 4*size;
	}

	specInfo.mapEntryCount = s;
	specInfo.pMapEntries = specentries;
	specInfo.dataSize = sizeof(specdata);
	specInfo.pData = &specdata;

#if 0//def _DEBUG
	//vk_layer_lunarg_drawstate fucks up and pokes invalid bits of stack.
	bugsbeware = Z_Malloc(sizeof(*bugsbeware) + sizeof(*specentries)*s + sizeof(specdata));
	*bugsbeware = specInfo;
	bugsbeware->pData = bugsbeware+1;
	bugsbeware->pMapEntries = (VkSpecializationMapEntry*)((char*)bugsbeware->pData + specInfo.dataSize);
	memcpy((void*)bugsbeware->pData, specInfo.pData, specInfo.dataSize);
	memcpy((void*)bugsbeware->pMapEntries, specInfo.pMapEntries, sizeof(*specInfo.pMapEntries)*specInfo.mapEntryCount);
#else
	bugsbeware = &specInfo;
#endif
	//fixme: add more specialisations for custom cvars (yes, this'll flush+reload pipelines if they're changed)
	//fixme: add specialisations for permutations I guess
	//fixme: add geometry+tesselation support. because we can.

	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = p->vert;
	shaderStages[0].pName = "main";
	shaderStages[0].pSpecializationInfo = bugsbeware;
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = p->frag;
	shaderStages[1].pName = "main";
	shaderStages[1].pSpecializationInfo = bugsbeware;

	pipeCreateInfo.flags				= 0;
	pipeCreateInfo.stageCount			= countof(shaderStages);
	pipeCreateInfo.pStages				= shaderStages;
	pipeCreateInfo.pVertexInputState	= &vi;
	pipeCreateInfo.pInputAssemblyState	= &ia;
	pipeCreateInfo.pTessellationState	= NULL;	//null is okay!
	pipeCreateInfo.pViewportState		= &vp;
	pipeCreateInfo.pRasterizationState	= &rs;
	pipeCreateInfo.pMultisampleState	= &ms;
	pipeCreateInfo.pDepthStencilState	= &ds;
	pipeCreateInfo.pColorBlendState		= &cb;
	pipeCreateInfo.pDynamicState		= &dyn;
	pipeCreateInfo.layout				= p->layout;
	pipeCreateInfo.renderPass			= (permu&PERMUTATION_BEM_DEPTHONLY)?vk.shadow_renderpass:vk.renderpass[0];
	pipeCreateInfo.subpass				= 0;
	pipeCreateInfo.basePipelineHandle	= VK_NULL_HANDLE;
	pipeCreateInfo.basePipelineIndex	= -1;	//not sure what this is about.

//				pipeCreateInfo.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	err = vkCreateGraphicsPipelines(vk.device, vk.pipelinecache, 1, &pipeCreateInfo, vkallocationcb, &pipe->pipeline);

	if (err)
	{
		shaderstate.activepipeline = VK_NULL_HANDLE;
		if (err != VK_ERROR_INVALID_SHADER_NV)
			Sys_Error("Error %i creating pipeline for %s. Check spir-v modules / drivers.\n", err, shaderstate.curshader->name);
		else
			Con_Printf("Error creating pipeline for %s. Check glsl / spir-v modules / drivers.\n", shaderstate.curshader->name);
		return;
	}

	vkCmdBindPipeline(vk.frame->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, shaderstate.activepipeline=pipe->pipeline);
}
static void BE_BindPipeline(program_t *p, unsigned int shaderflags, unsigned int blendflags, unsigned int permu)
{
	struct pipeline_s *pipe;
	blendflags &=	0
					| SBITS_SRCBLEND_BITS | SBITS_DSTBLEND_BITS | SBITS_MASK_BITS | SBITS_ATEST_BITS
					| SBITS_MISC_DEPTHWRITE | SBITS_MISC_NODEPTHTEST | SBITS_MISC_DEPTHEQUALONLY | SBITS_MISC_DEPTHCLOSERONLY
					| SBITS_LINES
					;
	shaderflags &= 0
					| SHADER_CULL_FRONT | SHADER_CULL_BACK
					| SHADER_POLYGONOFFSET
					;
	permu |= shaderstate.modepermutation;

	if (shaderflags & (SHADER_CULL_FRONT | SHADER_CULL_BACK))
		shaderflags ^= r_refdef.flipcull;

	for (pipe = p->pipelines; pipe; pipe = pipe->next)
	{
		if (pipe->flags == shaderflags)
			if (pipe->blendbits == blendflags)
				if (pipe->permu == permu)
				{
					if (pipe->pipeline != shaderstate.activepipeline)
					{
						shaderstate.activepipeline = pipe->pipeline;
						if (shaderstate.activepipeline)
							vkCmdBindPipeline(vk.frame->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, shaderstate.activepipeline);
					}
					return;
				}
	}

	//oh look. we need to build an entirely new pipeline object. hurrah... not.
	//split into a different function because of abusive stack combined with windows stack probes.
	BE_CreatePipeline(p, shaderflags, blendflags, permu);
}

static void BE_SetupTextureDescriptor(texid_t tex, texid_t fallbacktex, VkDescriptorSet set, VkWriteDescriptorSet *firstdesc, VkWriteDescriptorSet *desc, VkDescriptorImageInfo *img)
{
	if (!tex || !tex->vkimage)
		tex = fallbacktex;

	desc->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	desc->pNext = NULL;
	desc->dstSet = set;
	desc->dstBinding = desc-firstdesc;
	desc->dstArrayElement = 0;
	desc->descriptorCount = 1;
	desc->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	img->imageLayout = tex->vkimage->layout;
	img->imageView = tex->vkimage->view;
	img->sampler = tex->vkimage->sampler;
	desc->pImageInfo = img;
	desc->pBufferInfo = NULL;
	desc->pTexelBufferView = NULL;
}
static void BE_SetupUBODescriptor(VkDescriptorSet set, VkWriteDescriptorSet *firstdesc, VkWriteDescriptorSet *desc, VkDescriptorBufferInfo *info)
{
	desc->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	desc->pNext = NULL;
	desc->dstSet = set;
	desc->dstBinding = desc-firstdesc;
	desc->dstArrayElement = 0;
	desc->descriptorCount = 1;
	desc->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	desc->pImageInfo = NULL;
	desc->pBufferInfo = info;
	desc->pTexelBufferView = NULL;
}

static qboolean BE_SetupMeshProgram(program_t *p, shaderpass_t *pass, unsigned int shaderbits, unsigned int idxcount)
{
	int perm = 0;
	if (!p)
		return false;

	if (TEXLOADED(shaderstate.curtexnums->bump))
		perm |= PERMUTATION_BUMPMAP;
	if (TEXLOADED(shaderstate.curtexnums->fullbright))
		perm |= PERMUTATION_FULLBRIGHT;
	if (TEXLOADED(shaderstate.curtexnums->upperoverlay) || TEXLOADED(shaderstate.curtexnums->loweroverlay))
		perm |= PERMUTATION_UPPERLOWER;
	if (r_refdef.globalfog.density)
		perm |= PERMUTATION_FOG;
//	if (r_glsl_offsetmapping.ival && TEXLOADED(shaderstate.curtexnums->bump))
//		perm |= PERMUTATION_OFFSET;
	perm &= p->supportedpermutations;

	BE_BindPipeline(p, shaderbits, VKBE_ApplyShaderBits(pass->shaderbits), perm);
	if (!shaderstate.activepipeline)
		return false;	//err, something bad happened.

	//most gpus will have a fairly low descriptor set limit of 4 (this is the minimum required)
	//that isn't enough for all our textures, so we need to make stuff up as required.
	{
		VkDescriptorSet set = shaderstate.descriptorsets[0] = VKBE_TempDescriptorSet(p->desclayout);
		VkWriteDescriptorSet descs[MAX_TMUS], *desc = descs;
		VkDescriptorImageInfo imgs[MAX_TMUS], *img = imgs;
		unsigned int i;
		//why do I keep wanting to write 'desk'? its quite annoying.

		//light / scene
		BE_SetupUBODescriptor(set, descs, desc++, &shaderstate.ubo_entity);
		BE_SetupUBODescriptor(set, descs, desc++, &shaderstate.ubo_light);
		if (p->defaulttextures & (1u<<0))
			BE_SetupTextureDescriptor(shaderstate.currentshadowmap, r_whiteimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<1))
			BE_SetupTextureDescriptor(shaderstate.curdlight?shaderstate.curdlight->cubetexture:r_nulltex, r_whiteimage, set, descs, desc++, img++);

		//material
		if (p->defaulttextures & (1u<<2))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->base, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<3))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->bump, missing_texture_normal, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<4))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->specular, missing_texture_gloss, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<5))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->upperoverlay, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<6))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->loweroverlay, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<7))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->fullbright, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<8))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->paletted, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<9))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->reflectcube, r_blackimage, set, descs, desc++, img++);
		if (p->defaulttextures & (1u<<10))
			BE_SetupTextureDescriptor(shaderstate.curtexnums->reflectmask, r_whiteimage, set, descs, desc++, img++);

		//batch
		if (p->defaulttextures & (1u<<11))
		{
			unsigned int lmi = shaderstate.curbatch->lightmap[0];
			BE_SetupTextureDescriptor((lmi<numlightmaps)?lightmap[lmi]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
		}
		if (p->defaulttextures & (1u<<12))
		{
			texid_t delux = NULL;
			unsigned int lmi = shaderstate.curbatch->lightmap[0];
			if (lmi<numlightmaps && lightmap[lmi]->hasdeluxe)
				delux = lightmap[lmi+1]->lightmap_texture;
			BE_SetupTextureDescriptor(delux, r_whiteimage, set, descs, desc++, img++);
		}
#if MAXRLIGHTMAPS > 1
		if (p->defaulttextures & ((1u<<13)|(1u<<14)|(1u<<15)))
		{
			int lmi = shaderstate.curbatch->lightmap[1];
			BE_SetupTextureDescriptor((lmi<numlightmaps)?lightmap[lmi]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
			lmi = shaderstate.curbatch->lightmap[2];
			BE_SetupTextureDescriptor((lmi<numlightmaps)?lightmap[lmi]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
			lmi = shaderstate.curbatch->lightmap[3];
			BE_SetupTextureDescriptor((lmi<numlightmaps)?lightmap[lmi]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
		}
		if (p->defaulttextures & ((1u<<16)|(1u<<17)|(1u<<18)))
		{
			int lmi = shaderstate.curbatch->lightmap[1];
			if (lmi<numlightmaps && lightmap[lmi]->hasdeluxe)
			{
				BE_SetupTextureDescriptor((lmi+1<numlightmaps)?lightmap[lmi+1]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
				lmi = shaderstate.curbatch->lightmap[2];
				BE_SetupTextureDescriptor((lmi+1<numlightmaps)?lightmap[lmi+1]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
				lmi = shaderstate.curbatch->lightmap[3];
				BE_SetupTextureDescriptor((lmi+1<numlightmaps)?lightmap[lmi+1]->lightmap_texture:NULL, r_whiteimage, set, descs, desc++, img++);
			}
			else
			{
				BE_SetupTextureDescriptor(NULL, r_whiteimage, set, descs, desc++, img++);
				BE_SetupTextureDescriptor(NULL, r_whiteimage, set, descs, desc++, img++);
				BE_SetupTextureDescriptor(NULL, r_whiteimage, set, descs, desc++, img++);
			}
		}
#endif

		//shader / pass
		for (i = 0; i < p->numsamplers; i++)
			BE_SetupTextureDescriptor(SelectPassTexture(pass+i), r_blackimage, set, descs, desc++, img++);

		vkUpdateDescriptorSets(vk.device, desc-descs, descs, 0, NULL);
	}
	vkCmdBindDescriptorSets(vk.frame->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, countof(shaderstate.descriptorsets), shaderstate.descriptorsets, 0, NULL);

	RQuantAdd(RQUANT_PRIMITIVEINDICIES, idxcount);
	RQuantAdd(RQUANT_DRAWS, 1);

	return true;
}

static void BE_DrawMeshChain_Internal(void)
{
	shader_t *altshader;
	unsigned int vertcount, idxcount, idxfirst;
	mesh_t *m;
	qboolean vblends;	//software
//	void *map;
//	int i;
	unsigned int mno;
	unsigned int passno;
	//extern cvar_t r_polygonoffset_submodel_factor;
//	float pushdepth;
//	float pushfactor;

	//I wasn't going to do this... but gah.
	VkBuffer vertexbuffers[VK_BUFF_MAX];
	VkDeviceSize vertexoffsets[VK_BUFF_MAX];

	altshader = shaderstate.curshader;
	switch (shaderstate.mode)
	{
	case BEM_LIGHT:
		altshader = shaderstate.shader_rtlight[shaderstate.curlmode];
		break;
	case BEM_DEPTHONLY:
		altshader = shaderstate.curshader->bemoverrides[bemoverride_depthonly];
		if (!altshader)
			altshader = shaderstate.depthonly;
		break;
	case BEM_WIREFRAME:
		altshader = R_RegisterShader("wireframe", SUF_NONE, 
			"{\n"
				"{\n"
					"map $whiteimage\n"
				"}\n"
			"}\n"
			);
		break;
	default:
	case BEM_STANDARD:
		altshader = shaderstate.curshader;
		break;
	}
	if (!altshader)
		return;

	if (shaderstate.forcebeflags & BEF_FORCENODEPTH)
	{
		RQuantAdd(RQUANT_2DBATCHES, 1);
	}
	else if (shaderstate.curentity == &r_worldentity)
	{
		RQuantAdd(RQUANT_WORLDBATCHES, 1);
	}
	else
	{
		RQuantAdd(RQUANT_ENTBATCHES, 1);
	}

	if (altshader->flags & SHADER_HASCURRENTRENDER)
		T_Gen_CurrentRender();	//requires lots of pass-related work...

	//if this flag is set, then we have to generate our own arrays. to avoid processing extra verticies this may require that we re-pack the verts
	if (shaderstate.meshlist[0]->xyz2_array)// && !altshader->prog)
	{
		vblends = true;
		shaderstate.batchvbo = NULL;
	}
	else
	{
		vblends = false;
		if (altshader->flags & SHADER_NEEDSARRAYS)
			shaderstate.batchvbo = NULL;
		else if (shaderstate.curshader->numdeforms)
			shaderstate.batchvbo = NULL;
	}

	/*index buffers are common to all passes*/
	if (shaderstate.batchvbo)
	{
		/*however, we still want to try to avoid discontinuities, because that would otherwise be more draw calls. we can have gaps in verts though*/
		if (shaderstate.nummeshes == 1)
		{
			m = shaderstate.meshlist[0];

			vkCmdBindIndexBuffer(vk.frame->cbuf, shaderstate.batchvbo->indicies.vk.buff, shaderstate.batchvbo->indicies.vk.offs, VK_INDEX_TYPE);
			idxfirst = m->vbofirstelement;

			vertcount = m->vbofirstvert + m->numvertexes;
			idxcount = m->numindexes;
		}
		else if (0)//shaderstate.nummeshes == shaderstate.curbatch->maxmeshes)
		{
			idxfirst = 0;
			vertcount = shaderstate.batchvbo->vertcount;
			idxcount = shaderstate.batchvbo->indexcount;

			vkCmdBindIndexBuffer(vk.frame->cbuf, shaderstate.batchvbo->indicies.vk.buff, shaderstate.batchvbo->indicies.vk.offs, VK_INDEX_TYPE);
		}
		else
		{
			index_t *map;
			VkBuffer buf;
			unsigned int i;
			VkDeviceSize offset;
			vertcount = shaderstate.batchvbo->vertcount;
			for (mno = 0, idxcount = 0; mno < shaderstate.nummeshes; mno++)
			{
				m = shaderstate.meshlist[mno];
				idxcount += m->numindexes;
			}
			map = VKBE_AllocateBufferSpace(DB_EBO, idxcount * sizeof(*map), &buf, &offset);
			for (mno = 0; mno < shaderstate.nummeshes; mno++)
			{
				m = shaderstate.meshlist[mno];
				for (i = 0; i < m->numindexes; i++)
					map[i] = m->indexes[i]+m->vbofirstvert;
				map += m->numindexes;
			}
			vkCmdBindIndexBuffer(vk.frame->cbuf, buf, offset, VK_INDEX_TYPE);
			idxfirst = 0;
		}
	}
	else
	{	/*we're going to be using dynamic array stuff here, so generate an index array list that has no vertex gaps*/
		index_t *map;
		VkBuffer buf;
		unsigned int i;
		VkDeviceSize offset;
		for (mno = 0, vertcount = 0, idxcount = 0; mno < shaderstate.nummeshes; mno++)
		{
			m = shaderstate.meshlist[mno];
			vertcount += m->numvertexes;
			idxcount += m->numindexes;
		}

		map = VKBE_AllocateBufferSpace(DB_EBO, idxcount * sizeof(*map), &buf, &offset);
		for (mno = 0, vertcount = 0; mno < shaderstate.nummeshes; mno++)
		{
			m = shaderstate.meshlist[mno];
			if (!vertcount)
				memcpy(map, m->indexes, sizeof(index_t)*m->numindexes);
			else
			{
				for (i = 0; i < m->numindexes; i++)
					map[i] = m->indexes[i]+vertcount;
			}
			map += m->numindexes;
			vertcount += m->numvertexes;
		}
		vkCmdBindIndexBuffer(vk.frame->cbuf, buf, offset, VK_INDEX_TYPE);
		idxfirst = 0;
	}

	/*vertex buffers are common to all passes*/
	if (shaderstate.batchvbo && !vblends)
	{
		vertexbuffers[VK_BUFF_POS] = shaderstate.batchvbo->coord.vk.buff;
		vertexoffsets[VK_BUFF_POS] = shaderstate.batchvbo->coord.vk.offs;
	}
	else
	{
		vecV_t *map;
		const mesh_t *m;
		unsigned int mno;
		unsigned int i;

		map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vecV_t), &vertexbuffers[VK_BUFF_POS], &vertexoffsets[VK_BUFF_POS]);
		
		if (vblends)
		{
			for (mno = 0; mno < shaderstate.nummeshes; mno++)
			{
				const mesh_t *m = shaderstate.meshlist[mno];
				vecV_t *ov = shaderstate.curshader->numdeforms?tmpbuf:map;
				vecV_t *iv1 = m->xyz_array;
				vecV_t *iv2 = m->xyz2_array;
				float w1 = m->xyz_blendw[0];
				float w2 = m->xyz_blendw[1];
				for (i = 0; i < m->numvertexes; i++)
				{
					ov[i][0] = iv1[i][0]*w1 + iv2[i][0]*w2;
					ov[i][1] = iv1[i][1]*w1 + iv2[i][1]*w2;
					ov[i][2] = iv1[i][2]*w1 + iv2[i][2]*w2;
				}
				if (shaderstate.curshader->numdeforms)
				{
					for (i = 0; i < shaderstate.curshader->numdeforms-1; i++)
						deformgen(&shaderstate.curshader->deforms[i], m->numvertexes, tmpbuf, tmpbuf, m);
					deformgen(&shaderstate.curshader->deforms[i], m->numvertexes, tmpbuf, map, m);
				}
				map += m->numvertexes;
			}
		}
		else if (shaderstate.curshader->numdeforms > 1)
		{	//horrible code, because multiple deforms would otherwise READ from the gpu memory
			for (mno = 0; mno < shaderstate.nummeshes; mno++)
			{
				m = shaderstate.meshlist[mno];
				deformgen(&shaderstate.curshader->deforms[0], m->numvertexes, m->xyz_array, tmpbuf, m);
				for (i = 1; i < shaderstate.curshader->numdeforms-1; i++)
					deformgen(&shaderstate.curshader->deforms[i], m->numvertexes, tmpbuf, tmpbuf, m);
				deformgen(&shaderstate.curshader->deforms[i], m->numvertexes, tmpbuf, map, m);
				map += m->numvertexes;
			}
		}
		else
		{
			for (mno = 0; mno < shaderstate.nummeshes; mno++)
			{
				m = shaderstate.meshlist[mno];
				deformgen(&shaderstate.curshader->deforms[0], m->numvertexes, m->xyz_array, map, m);
				map += m->numvertexes;
			}
		}
	}

	if (altshader->prog)
	{
		if (shaderstate.batchvbo)
		{
			vertexbuffers[VK_BUFF_COL] = shaderstate.batchvbo->colours[0].vk.buff;
			vertexoffsets[VK_BUFF_COL] = shaderstate.batchvbo->colours[0].vk.offs;
			vertexbuffers[VK_BUFF_TC]  = shaderstate.batchvbo->texcoord.vk.buff;
			vertexoffsets[VK_BUFF_TC]  = shaderstate.batchvbo->texcoord.vk.offs;
			vertexbuffers[VK_BUFF_LMTC]= shaderstate.batchvbo->lmcoord[0].vk.buff;
			vertexoffsets[VK_BUFF_LMTC]= shaderstate.batchvbo->lmcoord[0].vk.offs;

			vertexbuffers[VK_BUFF_NORM]= shaderstate.batchvbo->normals.vk.buff;
			vertexoffsets[VK_BUFF_NORM]= shaderstate.batchvbo->normals.vk.offs;
			vertexbuffers[VK_BUFF_SDIR]= shaderstate.batchvbo->svector.vk.buff;
			vertexoffsets[VK_BUFF_SDIR]= shaderstate.batchvbo->svector.vk.offs;
			vertexbuffers[VK_BUFF_TDIR]= shaderstate.batchvbo->tvector.vk.buff;
			vertexoffsets[VK_BUFF_TDIR]= shaderstate.batchvbo->tvector.vk.offs;

			if (!vertexbuffers[VK_BUFF_COL])
			{
				vertexbuffers[VK_BUFF_COL] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_COL] = 0;
			}
			if (!vertexbuffers[VK_BUFF_LMTC])
			{
				vertexbuffers[VK_BUFF_LMTC] = vertexbuffers[VK_BUFF_TC];
				vertexoffsets[VK_BUFF_LMTC] = vertexoffsets[VK_BUFF_TC];
			}
		}
		else
		{
			vec2_t *map;
			vec2_t *lmmap;
			const mesh_t *m;
			unsigned int mno;
			unsigned int i;

			if (shaderstate.meshlist[0]->normals_array[0])
			{
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec3_t), &vertexbuffers[VK_BUFF_NORM], &vertexoffsets[VK_BUFF_NORM]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->normals_array[0], sizeof(vec3_t)*m->numvertexes);
					map += m->numvertexes;
				}
			}
			else
			{
				vertexbuffers[VK_BUFF_NORM] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_NORM] = sizeof(vec4_t)*65536;
			}

			if (shaderstate.meshlist[0]->snormals_array[0])
			{
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec3_t), &vertexbuffers[VK_BUFF_SDIR], &vertexoffsets[VK_BUFF_SDIR]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->snormals_array[0], sizeof(vec3_t)*m->numvertexes);
					map += m->numvertexes;
				}
			}
			else
			{
				vertexbuffers[VK_BUFF_SDIR] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_SDIR] = sizeof(vec4_t)*65536 + sizeof(vec3_t)*65536;
			}

			if (shaderstate.meshlist[0]->tnormals_array[0])
			{
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec3_t), &vertexbuffers[VK_BUFF_TDIR], &vertexoffsets[VK_BUFF_TDIR]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->tnormals_array[0], sizeof(vec3_t)*m->numvertexes);
					map += m->numvertexes;
				}
			}
			else
			{
				vertexbuffers[VK_BUFF_TDIR] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_TDIR] = sizeof(vec4_t)*65536 + sizeof(vec3_t)*65536 + sizeof(vec3_t)*65536;
			}

			if (shaderstate.meshlist[0]->colors4f_array[0])
			{
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec4_t), &vertexbuffers[VK_BUFF_COL], &vertexoffsets[VK_BUFF_COL]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->colors4f_array[0], sizeof(vec4_t)*m->numvertexes);
					map += m->numvertexes;
				}
			}
			else if (shaderstate.meshlist[0]->colors4b_array)
			{
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec4_t), &vertexbuffers[VK_BUFF_COL], &vertexoffsets[VK_BUFF_COL]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					for (i = 0; i < m->numvertexes; i++)
					{
						Vector4Scale(m->colors4b_array[i], (1/255.0), map[i]);
					}
					map += m->numvertexes;
				}
			}
			else
			{	//FIXME: use some predefined buffer
				vec4_t *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec4_t), &vertexbuffers[VK_BUFF_COL], &vertexoffsets[VK_BUFF_COL]);
				for (i = 0; i < vertcount; i++)
				{
					Vector4Set(map[i], 1, 1, 1, 1);
				}
			}

			if (shaderstate.meshlist[0]->lmst_array[0])
			{
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec2_t), &vertexbuffers[VK_BUFF_TC], &vertexoffsets[VK_BUFF_TC]);
				lmmap = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec2_t), &vertexbuffers[VK_BUFF_LMTC], &vertexoffsets[VK_BUFF_LMTC]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->st_array, sizeof(vec2_t)*m->numvertexes);
					memcpy(lmmap, m->lmst_array[0], sizeof(vec2_t)*m->numvertexes);
					map += m->numvertexes;
					lmmap += m->numvertexes;
				}
			}
			else
			{
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec2_t), &vertexbuffers[VK_BUFF_TC], &vertexoffsets[VK_BUFF_TC]);
				for (mno = 0; mno < shaderstate.nummeshes; mno++)
				{
					m = shaderstate.meshlist[mno];
					memcpy(map, m->st_array, sizeof(*m->st_array)*m->numvertexes);
					map += m->numvertexes;
				}
				
				vertexbuffers[VK_BUFF_LMTC] = vertexbuffers[VK_BUFF_TC];
				vertexoffsets[VK_BUFF_LMTC] = vertexoffsets[VK_BUFF_TC];
			}
		}

		vkCmdBindVertexBuffers(vk.frame->cbuf, 0, VK_BUFF_MAX, vertexbuffers, vertexoffsets);
		if (BE_SetupMeshProgram(altshader->prog, altshader->passes, altshader->flags, idxcount))
			vkCmdDrawIndexed(vk.frame->cbuf, idxcount, 1, idxfirst, 0, 0);
	}
	else if (1)
	{
		shaderpass_t *p;

		//Vulkan has no fixed function pipeline. we emulate it if we were given no spir-v to run.

		for (passno = 0; passno < altshader->numpasses; passno += p->numMergedPasses)
		{
			p = &altshader->passes[passno];

			if (p->texgen == T_GEN_UPPEROVERLAY && !TEXLOADED(shaderstate.curtexnums->upperoverlay))
				continue;
			if (p->texgen == T_GEN_LOWEROVERLAY && !TEXLOADED(shaderstate.curtexnums->loweroverlay))
				continue;
			if (p->texgen == T_GEN_FULLBRIGHT && !TEXLOADED(shaderstate.curtexnums->fullbright))
				continue;

			if (p->prog)
			{
				vertexbuffers[VK_BUFF_TC] = shaderstate.batchvbo->texcoord.vk.buff;
				vertexoffsets[VK_BUFF_TC] = shaderstate.batchvbo->texcoord.vk.offs;
				vertexbuffers[VK_BUFF_LMTC] = shaderstate.batchvbo->lmcoord[0].vk.buff;
				vertexoffsets[VK_BUFF_LMTC] = shaderstate.batchvbo->lmcoord[0].vk.offs;

				BE_GenerateColourMods(vertcount, p, &vertexbuffers[VK_BUFF_COL], &vertexoffsets[VK_BUFF_COL]);

				vertexbuffers[VK_BUFF_NORM] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_NORM] = sizeof(vec4_t)*65536;
				vertexbuffers[VK_BUFF_SDIR] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_SDIR] = vertexoffsets[VK_BUFF_NORM] + sizeof(vec3_t)*65536;
				vertexbuffers[VK_BUFF_TDIR] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_TDIR] = vertexoffsets[VK_BUFF_SDIR] + sizeof(vec3_t)*65536;

				vkCmdBindVertexBuffers(vk.frame->cbuf, 0, VK_BUFF_MAX, vertexbuffers, vertexoffsets);
				if (BE_SetupMeshProgram(p->prog, p, altshader->flags, idxcount))
					vkCmdDrawIndexed(vk.frame->cbuf, idxcount, 1, idxfirst, 0, 0);
				continue;
			}

			if (shaderstate.batchvbo)
			{	//texcoords are all compatible with static arrays, supposedly
				if (p->tcgen == TC_GEN_LIGHTMAP)
				{
					vertexbuffers[VK_BUFF_TC] = shaderstate.batchvbo->lmcoord[0].vk.buff;
					vertexoffsets[VK_BUFF_TC] = shaderstate.batchvbo->lmcoord[0].vk.offs;
				}
				else if (p->tcgen == TC_GEN_BASE)
				{
					vertexbuffers[VK_BUFF_TC] = shaderstate.batchvbo->texcoord.vk.buff;
					vertexoffsets[VK_BUFF_TC] = shaderstate.batchvbo->texcoord.vk.offs;
				}
				else
					Sys_Error("tcgen %u not supported\n", p->tcgen);
			}
			else
			{
				float *map;
				map = VKBE_AllocateBufferSpace(DB_VBO, vertcount * sizeof(vec2_t), &vertexbuffers[VK_BUFF_TC], &vertexoffsets[VK_BUFF_TC]);
				BE_GenerateTCMods(p, map);
			}

			vertexbuffers[VK_BUFF_LMTC] = vertexbuffers[VK_BUFF_TC];
			vertexoffsets[VK_BUFF_LMTC] = vertexoffsets[VK_BUFF_TC];

			vertexbuffers[VK_BUFF_NORM] = shaderstate.staticbuf;
			vertexoffsets[VK_BUFF_NORM] = sizeof(vec4_t)*65536;
			vertexbuffers[VK_BUFF_SDIR] = shaderstate.staticbuf;
			vertexoffsets[VK_BUFF_SDIR] = vertexoffsets[VK_BUFF_NORM] + sizeof(vec3_t)*65536;
			vertexbuffers[VK_BUFF_TDIR] = shaderstate.staticbuf;
			vertexoffsets[VK_BUFF_TDIR] = vertexoffsets[VK_BUFF_SDIR] + sizeof(vec3_t)*65536;

			if (p->flags & SHADER_PASS_NOCOLORARRAY)
			{
				avec4_t passcolour;
				static avec4_t fakesource = {1,1,1,1};
				m = shaderstate.meshlist[0];
				colourgen(p, 1, NULL, &fakesource, &passcolour, m);
				alphagen(p, 1, NULL, &fakesource, &passcolour, m);

				//make sure nothing bugs out... this should be pure white.
				vertexbuffers[VK_BUFF_COL] = shaderstate.staticbuf;
				vertexoffsets[VK_BUFF_COL] = 0;

				vkCmdBindVertexBuffers(vk.frame->cbuf, 0, VK_BUFF_MAX, vertexbuffers, vertexoffsets);
				if (BE_SetupMeshProgram(shaderstate.programfixedemu[1], p, altshader->flags, idxcount))
				{
					vkCmdPushConstants(vk.frame->cbuf, shaderstate.programfixedemu[1]->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(passcolour), passcolour);
					vkCmdDrawIndexed(vk.frame->cbuf, idxcount, 1, idxfirst, 0, 0);
				}
			}
			else
			{
				BE_GenerateColourMods(vertcount, p, &vertexbuffers[VK_BUFF_COL], &vertexoffsets[VK_BUFF_COL]);
				vkCmdBindVertexBuffers(vk.frame->cbuf, 0, VK_BUFF_MAX, vertexbuffers, vertexoffsets);
				if (BE_SetupMeshProgram(shaderstate.programfixedemu[0], p, altshader->flags, idxcount))
					vkCmdDrawIndexed(vk.frame->cbuf, idxcount, 1, idxfirst, 0, 0);
			}
		}
	}
}

void VKBE_SelectMode(backendmode_t mode)
{
	shaderstate.mode = mode;
	shaderstate.modepermutation = 0;

	switch(mode)
	{
	default:
		break;

	case BEM_DEPTHONLY:
		shaderstate.modepermutation |= PERMUTATION_BEM_DEPTHONLY;
		break;

	case BEM_WIREFRAME:
		shaderstate.modepermutation |= PERMUTATION_BEM_WIREFRAME;
		break;

	case BEM_LIGHT:
		//fixme: is this actually needed, or just a waste of time?
		VKBE_SelectEntity(&r_worldentity);
		break;
	}
}
qboolean VKBE_GenerateRTLightShader(unsigned int lmode)
{
	if (!shaderstate.shader_rtlight[lmode])
	{
		shaderstate.shader_rtlight[lmode] = R_RegisterShader(va("rtlight%s%s%s", 
															(lmode & LSHADER_SMAP)?"#PCF=1":"#PCF=0",
															(lmode & LSHADER_SPOT)?"#SPOT=1":"#SPOT=0",
															(lmode & LSHADER_CUBE)?"#CUBE=1":"#CUBE=0")
														, SUF_NONE, LIGHTPASS_SHADER);
	}
	if (shaderstate.shader_rtlight[lmode]->flags & SHADER_NODRAW)
		return false;
	return true;
}
qboolean VKBE_SelectDLight(dlight_t *dl, vec3_t colour, vec3_t axis[3], unsigned int lmode)
{
	if (dl && TEXLOADED(dl->cubetexture))
		lmode |= LSHADER_CUBE;

	if (!VKBE_GenerateRTLightShader(lmode))
	{
		lmode &= ~(LSHADER_SMAP|LSHADER_CUBE);
		if (!VKBE_GenerateRTLightShader(lmode))
		{
			VKBE_SetupLightCBuffer(NULL, colour);
			return false;
		}
	}
	shaderstate.curdlight = dl;
	shaderstate.curlmode = lmode;

	VKBE_SetupLightCBuffer(dl, colour);
	return true;
}

void VKBE_SelectEntity(entity_t *ent)
{
	BE_RotateForEntity(ent, ent->model);
}

//fixme: create allocations within larger buffers, use separate staging.
void *VKBE_CreateStagingBuffer(struct stagingbuf *n, size_t size, VkBufferUsageFlags usage)
{
	void *ptr;
	VkBufferCreateInfo bufinf = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	VkMemoryRequirements mem_reqs;
	VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

	n->retbuf = VK_NULL_HANDLE;
	n->usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufinf.flags = 0;
	bufinf.size = n->size = size;
	bufinf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufinf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bufinf.queueFamilyIndexCount = 0;
	bufinf.pQueueFamilyIndices = NULL;
	vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &n->buf);

	vkGetBufferMemoryRequirements(vk.device, n->buf, &mem_reqs);

	memAllocInfo.allocationSize = mem_reqs.size;
	memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	if (memAllocInfo.memoryTypeIndex == ~0)
		Sys_Error("Unable to allocate buffer memory");

	VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &n->memory));
	VkAssert(vkBindBufferMemory(vk.device, n->buf, n->memory, 0));
	VkAssert(vkMapMemory(vk.device, n->memory, 0, n->size, 0, &ptr));

	return ptr;
}

struct fencedbufferwork
{
	struct vk_fencework fw;

	VkBuffer buf;
	VkDeviceMemory mem;
};
static void VKBE_DoneBufferStaging(void *staging)
{
	struct fencedbufferwork *n = staging;
	vkDestroyBuffer(vk.device, n->buf, vkallocationcb);
	vkFreeMemory(vk.device, n->mem, vkallocationcb);
}
VkBuffer VKBE_FinishStaging(struct stagingbuf *n, VkDeviceMemory *memptr)
{
	struct fencedbufferwork *fence;
	VkBuffer retbuf;
	
	//caller filled the staging buffer, and now wants to copy stuff to the gpu.
	vkUnmapMemory(vk.device, n->memory);

	//create the hardware buffer
	if (n->retbuf)
		retbuf = n->retbuf;
	else
	{
		VkBufferCreateInfo bufinf = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

		bufinf.flags = 0;
		bufinf.size = n->size;
		bufinf.usage = n->usage;
		bufinf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bufinf.queueFamilyIndexCount = 0;
		bufinf.pQueueFamilyIndices = NULL;
		vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &retbuf);
	}

	//sort out its memory
	{
		VkMemoryRequirements mem_reqs;
		VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		vkGetBufferMemoryRequirements(vk.device, retbuf, &mem_reqs);
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (memAllocInfo.memoryTypeIndex == ~0)
			Sys_Error("Unable to allocate buffer memory");
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, memptr));
		VkAssert(vkBindBufferMemory(vk.device, retbuf, *memptr, 0));
	}
	

	fence = VK_FencedBegin(VKBE_DoneBufferStaging, sizeof(*fence));
	fence->buf = n->buf;
	fence->mem = n->memory;

	//FIXME: barrier?

	//add the copy command
	{
		VkBufferCopy bcr = {0};
		bcr.srcOffset = 0;
		bcr.dstOffset = 0;
		bcr.size = n->size;
		vkCmdCopyBuffer(fence->fw.cbuf, n->buf, retbuf, 1, &bcr);
	}

	//FIXME: barrier?

	VK_FencedSubmit(fence);

	return retbuf;
}

void VKBE_GenBatchVBOs(vbo_t **vbochain, batch_t *firstbatch, batch_t *stopbatch)
{
	int maxvboelements;
	int maxvboverts;
	int vert = 0, idx = 0;
	batch_t *batch;
	vbo_t *vbo;
	int i, j;
	mesh_t *m;
	index_t *vboedata;
	qbyte *vbovdatastart, *vbovdata;
	struct stagingbuf vbuf, ebuf;
	VkDeviceMemory *retarded;

	vbo = Z_Malloc(sizeof(*vbo));

	maxvboverts = 0;
	maxvboelements = 0;
	for(batch = firstbatch; batch != stopbatch; batch = batch->next)
	{
		for (i=0 ; i<batch->maxmeshes ; i++)
		{
			m = batch->mesh[i];
			maxvboelements += m->numindexes;
			maxvboverts += m->numvertexes;
		}
	}

	if (!maxvboverts || !maxvboelements)
		return;

	//determine array offsets.
	vbovdatastart = vbovdata = NULL;
	vbo->coord.vk.offs = vbovdata-vbovdatastart;		vbovdata += sizeof(vecV_t)*maxvboverts;
	vbo->texcoord.vk.offs = vbovdata-vbovdatastart;		vbovdata += sizeof(vec2_t)*maxvboverts;
	vbo->lmcoord[0].vk.offs = vbovdata-vbovdatastart;	vbovdata += sizeof(vec2_t)*maxvboverts;
	vbo->normals.vk.offs = vbovdata-vbovdatastart;		vbovdata += sizeof(vec3_t)*maxvboverts;
	vbo->svector.vk.offs = vbovdata-vbovdatastart;		vbovdata += sizeof(vec3_t)*maxvboverts;
	vbo->tvector.vk.offs = vbovdata-vbovdatastart;		vbovdata += sizeof(vec3_t)*maxvboverts;
	vbo->colours[0].vk.offs = vbovdata-vbovdatastart;	vbovdata += sizeof(vec4_t)*maxvboverts;

	vbovdatastart = vbovdata = VKBE_CreateStagingBuffer(&vbuf, vbovdata-vbovdatastart, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	vboedata = VKBE_CreateStagingBuffer(&ebuf, sizeof(*vboedata) * maxvboelements, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

	vbo->indicies.vk.offs = 0;

	for(batch = firstbatch; batch != stopbatch; batch = batch->next)
	{
		batch->vbo = vbo;
		for (j=0 ; j<batch->maxmeshes ; j++)
		{
			m = batch->mesh[j];
			m->vbofirstvert = vert;

			if (m->xyz_array)
				memcpy(vbovdata + vbo->coord.vk.offs		+ vert*sizeof(vecV_t), m->xyz_array,		sizeof(vecV_t)*m->numvertexes);
			if (m->st_array)
				memcpy(vbovdata + vbo->texcoord.vk.offs		+ vert*sizeof(vec2_t), m->st_array,			sizeof(vec2_t)*m->numvertexes);
			if (m->lmst_array[0])
				memcpy(vbovdata + vbo->lmcoord[0].vk.offs	+ vert*sizeof(vec2_t), m->lmst_array[0],	sizeof(vec2_t)*m->numvertexes);
			if (m->normals_array)
				memcpy(vbovdata + vbo->normals.vk.offs		+ vert*sizeof(vec3_t), m->normals_array,	sizeof(vec3_t)*m->numvertexes);
			if (m->snormals_array)
				memcpy(vbovdata + vbo->svector.vk.offs		+ vert*sizeof(vec3_t), m->snormals_array,	sizeof(vec3_t)*m->numvertexes);
			if (m->tnormals_array)
				memcpy(vbovdata + vbo->tvector.vk.offs		+ vert*sizeof(vec3_t), m->tnormals_array,	sizeof(vec3_t)*m->numvertexes);
			if (m->colors4f_array[0])
				memcpy(vbovdata + vbo->colours[0].vk.offs	+ vert*sizeof(vec4_t), m->colors4f_array[0],sizeof(vec4_t)*m->numvertexes);

			m->vbofirstelement = idx;
			for (i = 0; i < m->numindexes; i++)
			{
				*vboedata++ = vert + m->indexes[i];
			}
			idx += m->numindexes;
			vert += m->numvertexes;
		}
	}

	vbo->vbomem = retarded = Z_Malloc(sizeof(*retarded));
	vbo->coord.vk.buff = 
	vbo->texcoord.vk.buff = 
	vbo->lmcoord[0].vk.buff = 
	vbo->normals.vk.buff = 
	vbo->svector.vk.buff = 
	vbo->tvector.vk.buff = 
	vbo->colours[0].vk.buff = VKBE_FinishStaging(&vbuf, retarded);

	vbo->ebomem = retarded = Z_Malloc(sizeof(*retarded));
	vbo->indicies.vk.buff = VKBE_FinishStaging(&ebuf, retarded);
	vbo->indicies.vk.offs = 0;

	vbo->indexcount = maxvboelements;
	vbo->vertcount = maxvboverts;

	vbo->next = *vbochain;
	*vbochain = vbo;
}

void VKBE_GenBrushModelVBO(model_t *mod)
{
	unsigned int vcount, cvcount;

	batch_t *batch, *fbatch;
	int sortid;
	int i;

	fbatch = NULL;
	vcount = 0;
	for (sortid = 0; sortid < SHADER_SORT_COUNT; sortid++)
	{
		if (!mod->batches[sortid])
			continue;

		for (fbatch = batch = mod->batches[sortid]; batch != NULL; batch = batch->next)
		{
			for (i = 0, cvcount = 0; i < batch->maxmeshes; i++)
				cvcount += batch->mesh[i]->numvertexes;

			if (vcount + cvcount > MAX_INDICIES)
			{
				VKBE_GenBatchVBOs(&mod->vbos, fbatch, batch);
				fbatch = batch;
				vcount = 0;
			}

			vcount += cvcount;
		}

		VKBE_GenBatchVBOs(&mod->vbos, fbatch, batch);
	}
}

/*Wipes a vbo*/
void VKBE_ClearVBO(vbo_t *vbo)
{
	//FIXME: may still be in use by an active commandbuffer.
	VkDeviceMemory *retarded;

	if (vbo->indicies.vk.buff || vbo->coord.vk.buff)
		vkDeviceWaitIdle(vk.device); //just in case

	if (vbo->indicies.vk.buff)
	{
		vkDestroyBuffer(vk.device, vbo->indicies.vk.buff, vkallocationcb);
		retarded = vbo->ebomem;
		vkFreeMemory(vk.device, *retarded, vkallocationcb);
		BZ_Free(retarded);
	}

	if (vbo->coord.vk.buff)
	{
		vkDestroyBuffer(vk.device, vbo->coord.vk.buff, vkallocationcb);
		retarded = vbo->vbomem;
		vkFreeMemory(vk.device, *retarded, vkallocationcb);
		BZ_Free(retarded);
	}

	BZ_Free(vbo);
}

void VK_UploadLightmap(lightmapinfo_t *lm)
{
	extern cvar_t gl_lightmap_nearest;
	struct pendingtextureinfo mips;
	image_t *tex;
	lm->modified = false;
	if (!TEXVALID(lm->lightmap_texture))
	{
		lm->lightmap_texture = Image_CreateTexture("***lightmap***", NULL, (gl_lightmap_nearest.ival?IF_NEAREST:IF_LINEAR));
		if (!lm->lightmap_texture)
			return;
	}
	tex = lm->lightmap_texture;

	mips.extrafree = NULL;
	mips.type = PTI_2D;
	mips.mip[0].data = lm->lightmaps;
	mips.mip[0].needfree = false;
	mips.mip[0].width = lm->width;
	mips.mip[0].height = lm->height;
	switch(lightmap_fmt)
	{
	case TF_BGRA32:
		mips.encoding = PTI_BGRX8;
		break;
	default:
		Sys_Error("Unsupported encoding\n");
		break;
	}
	mips.mipcount = 1;
	VK_LoadTextureMips(tex, &mips);
	tex->status = TEX_LOADED;
	tex->width = lm->width;
	tex->height = lm->height;

	lm->lightmap_texture = tex;
}
/*upload all lightmaps at the start to reduce lags*/
static void BE_UploadLightmaps(qboolean force)
{
	int i;

	for (i = 0; i < numlightmaps; i++)
	{
		if (!lightmap[i])
			continue;

		if (force)
		{
			lightmap[i]->rectchange.l = 0;
			lightmap[i]->rectchange.t = 0;
			lightmap[i]->rectchange.r = lightmap[i]->width;
			lightmap[i]->rectchange.b = lightmap[i]->height;
			lightmap[i]->modified = true;
		}

		if (lightmap[i]->modified)
		{
			VK_UploadLightmap(lightmap[i]);
		}
	}
}

void VKBE_UploadAllLightmaps(void)
{
	BE_UploadLightmaps(true);
}

qboolean VKBE_LightCullModel(vec3_t org, model_t *model)
{
#ifdef RTLIGHTS
	if ((shaderstate.mode == BEM_LIGHT || shaderstate.mode == BEM_STENCIL || shaderstate.mode == BEM_DEPTHONLY))
	{
		float dist;
		vec3_t disp;
		if (model->type == mod_alias)
		{
			VectorSubtract(org, shaderstate.lightinfo, disp);
			dist = DotProduct(disp, disp);
			if (dist > model->radius*model->radius + shaderstate.lightinfo[3]*shaderstate.lightinfo[3])
				return true;
		}
		else
		{
			int i;

			for (i = 0; i < 3; i++)
			{
				if (shaderstate.lightinfo[i]-shaderstate.lightinfo[3] > org[i] + model->maxs[i])
					return true;
				if (shaderstate.lightinfo[i]+shaderstate.lightinfo[3] < org[i] + model->mins[i])
					return true;
			}
		}
	}
#endif
	return false;
}

batch_t *VKBE_GetTempBatch(void)
{
	if (shaderstate.wbatch >= shaderstate.maxwbatches)
	{
		shaderstate.wbatch++;
		return NULL;
	}
	return &shaderstate.wbatches[shaderstate.wbatch++];
}

void VKBE_SetupLightCBuffer(dlight_t *l, vec3_t colour)
{
	extern cvar_t gl_specular;
	cbuf_light_t *cbl = VKBE_AllocateBufferSpace(DB_UBO, (sizeof(*cbl) + 0x0ff) & ~0xff, &shaderstate.ubo_light.buffer, &shaderstate.ubo_light.offset);
	shaderstate.ubo_light.range = sizeof(*cbl);

	if (!l)
	{
		memset(cbl, 0, sizeof(*cbl));

		Vector4Set(shaderstate.lightinfo, 0, 0, 0, 0);
		return;
	}


	cbl->l_lightradius = l->radius;

	if (shaderstate.curlmode & LSHADER_SPOT)
	{
		float view[16];
		float proj[16];
		extern cvar_t r_shadow_shadowmapping_nearclip;
		Matrix4x4_CM_Projection_Far(proj, l->fov, l->fov, r_shadow_shadowmapping_nearclip.value, l->radius);
		Matrix4x4_CM_ModelViewMatrixFromAxis(view, l->axis[0], l->axis[1], l->axis[2], l->origin);
		Matrix4_Multiply(proj, view, cbl->l_cubematrix);
	}
	else
		Matrix4x4_CM_LightMatrixFromAxis(cbl->l_cubematrix, l->axis[0], l->axis[1], l->axis[2], l->origin);
	VectorCopy(l->origin, cbl->l_lightposition);
	cbl->padl1 = 0;
	VectorCopy(colour, cbl->l_colour);
#ifdef RTLIGHTS
	VectorCopy(l->lightcolourscales, cbl->l_lightcolourscale);
	cbl->l_lightcolourscale[0] = l->lightcolourscales[0];
	cbl->l_lightcolourscale[1] = l->lightcolourscales[1];
	cbl->l_lightcolourscale[2] = l->lightcolourscales[2] * gl_specular.value;
#endif
	cbl->l_lightradius = l->radius;
	Vector4Copy(shaderstate.lightshadowmapproj, cbl->l_shadowmapproj);
	Vector2Copy(shaderstate.lightshadowmapscale, cbl->l_shadowmapscale);

	VectorCopy(l->origin, shaderstate.lightinfo);
	shaderstate.lightinfo[3] = l->radius;
}


//also updates the entity constant buffer
static void BE_RotateForEntity (const entity_t *e, const model_t *mod)
{
	int i;
	float ndr;
	float modelmatrix[16];
	float *m = modelmatrix;
	cbuf_entity_t *cbe = VKBE_AllocateBufferSpace(DB_UBO, (sizeof(*cbe) + 0x0ff) & ~0xff, &shaderstate.ubo_entity.buffer, &shaderstate.ubo_entity.offset);
	shaderstate.ubo_entity.range = sizeof(*cbe);

	shaderstate.curentity = e;


	if ((e->flags & RF_WEAPONMODEL) && r_refdef.playerview->viewentity > 0)
	{
		float em[16];
		float vm[16];

		if (e->flags & RF_WEAPONMODELNOBOB)
		{
			vm[0] = vpn[0];
			vm[1] = vpn[1];
			vm[2] = vpn[2];
			vm[3] = 0;

			vm[4] = -vright[0];
			vm[5] = -vright[1];
			vm[6] = -vright[2];
			vm[7] = 0;

			vm[8] = vup[0];
			vm[9] = vup[1];
			vm[10] = vup[2];
			vm[11] = 0;

			vm[12] = r_refdef.vieworg[0];
			vm[13] = r_refdef.vieworg[1];
			vm[14] = r_refdef.vieworg[2];
			vm[15] = 1;
		}
		else
		{
			vm[0] = r_refdef.playerview->vw_axis[0][0];
			vm[1] = r_refdef.playerview->vw_axis[0][1];
			vm[2] = r_refdef.playerview->vw_axis[0][2];
			vm[3] = 0;

			vm[4] = r_refdef.playerview->vw_axis[1][0];
			vm[5] = r_refdef.playerview->vw_axis[1][1];
			vm[6] = r_refdef.playerview->vw_axis[1][2];
			vm[7] = 0;

			vm[8] = r_refdef.playerview->vw_axis[2][0];
			vm[9] = r_refdef.playerview->vw_axis[2][1];
			vm[10] = r_refdef.playerview->vw_axis[2][2];
			vm[11] = 0;

			vm[12] = r_refdef.playerview->vw_origin[0];
			vm[13] = r_refdef.playerview->vw_origin[1];
			vm[14] = r_refdef.playerview->vw_origin[2];
			vm[15] = 1;
		}

		em[0] = e->axis[0][0];
		em[1] = e->axis[0][1];
		em[2] = e->axis[0][2];
		em[3] = 0;

		em[4] = e->axis[1][0];
		em[5] = e->axis[1][1];
		em[6] = e->axis[1][2];
		em[7] = 0;

		em[8] = e->axis[2][0];
		em[9] = e->axis[2][1];
		em[10] = e->axis[2][2];
		em[11] = 0;

		em[12] = e->origin[0];
		em[13] = e->origin[1];
		em[14] = e->origin[2];
		em[15] = 1;

		Matrix4_Multiply(vm, em, m);
	}
	else
	{
		m[0] = e->axis[0][0];
		m[1] = e->axis[0][1];
		m[2] = e->axis[0][2];
		m[3] = 0;

		m[4] = e->axis[1][0];
		m[5] = e->axis[1][1];
		m[6] = e->axis[1][2];
		m[7] = 0;

		m[8] = e->axis[2][0];
		m[9] = e->axis[2][1];
		m[10] = e->axis[2][2];
		m[11] = 0;

		m[12] = e->origin[0];
		m[13] = e->origin[1];
		m[14] = e->origin[2];
		m[15] = 1;
	}

	if (e->scale != 1 && e->scale != 0)	//hexen 2 stuff
	{
#ifdef HEXEN2
		float z;
		float escale;
		escale = e->scale;
		switch(e->drawflags&SCALE_TYPE_MASK)
		{
		default:
		case SCALE_TYPE_UNIFORM:
			VectorScale((m+0), escale, (m+0));
			VectorScale((m+4), escale, (m+4));
			VectorScale((m+8), escale, (m+8));
			break;
		case SCALE_TYPE_XYONLY:
			VectorScale((m+0), escale, (m+0));
			VectorScale((m+4), escale, (m+4));
			break;
		case SCALE_TYPE_ZONLY:
			VectorScale((m+8), escale, (m+8));
			break;
		}
		if (mod && (e->drawflags&SCALE_TYPE_MASK) != SCALE_TYPE_XYONLY)
		{
			switch(e->drawflags&SCALE_ORIGIN_MASK)
			{
			case SCALE_ORIGIN_CENTER:
				z = ((mod->maxs[2] + mod->mins[2]) * (1-escale))/2;
				VectorMA((m+12), z, e->axis[2], (m+12));
				break;
			case SCALE_ORIGIN_BOTTOM:
				VectorMA((m+12), mod->mins[2]*(1-escale), e->axis[2], (m+12));
				break;
			case SCALE_ORIGIN_TOP:
				VectorMA((m+12), -mod->maxs[2], e->axis[2], (m+12));
				break;
			}
		}
#else
		VectorScale((m+0), e->scale, (m+0));
		VectorScale((m+4), e->scale, (m+4));
		VectorScale((m+8), e->scale, (m+8));
#endif
	}
	else if (mod && !strcmp(mod->name, "progs/eyes.mdl"))
	{
		/*resize eyes, to make them easier to see*/
		m[14] -= (22 + 8);
		VectorScale((m+0), 2, (m+0));
		VectorScale((m+4), 2, (m+4));
		VectorScale((m+8), 2, (m+8));
	}
	if (mod && !ruleset_allow_larger_models.ival && mod->clampscale != 1)
	{	//possibly this should be on a per-frame basis, but that's a real pain to do
		Con_DPrintf("Rescaling %s by %f\n", mod->name, mod->clampscale);
		VectorScale((m+0), mod->clampscale, (m+0));
		VectorScale((m+4), mod->clampscale, (m+4));
		VectorScale((m+8), mod->clampscale, (m+8));
	}

	{
		float modelview[16];
		Matrix4_Multiply(r_refdef.m_view, m, modelview);
		Matrix4_Multiply(r_refdef.m_projection, modelview, cbe->m_modelviewproj);
	}
	memcpy(cbe->m_model, m, sizeof(cbe->m_model));
	Matrix4_Invert(modelmatrix, cbe->m_modelinv);
	Matrix4x4_CM_Transform3(cbe->m_modelinv, r_origin, cbe->e_eyepos);

	cbe->e_time = shaderstate.curtime = r_refdef.time - shaderstate.curentity->shaderTime;

	VectorCopy(e->light_avg, cbe->e_light_ambient);	cbe->pad1 = 0;
	VectorCopy(e->light_dir, cbe->e_light_dir);		cbe->pad2 = 0;
	VectorCopy(e->light_range, cbe->e_light_mul);	cbe->pad3 = 0;

	for (i = 0; i < MAXRLIGHTMAPS ; i++)
	{
		//FIXME: this is fucked, the batch isn't known yet.
		#if 0
		extern cvar_t gl_overbright;
		unsigned char s = shaderstate.curbatch?shaderstate.curbatch->lmlightstyle[i]:0;
		float sc;
		if (s == 255)
		{
			if (i == 0)
			{
				if (shaderstate.curentity->model && shaderstate.curentity->model->engineflags & MDLF_NEEDOVERBRIGHT)
					sc = (1<<bound(0, gl_overbright.ival, 2)) * shaderstate.identitylighting;
				else
					sc = shaderstate.identitylighting;
				cbe->e_lmscale[i][0] = sc;
				cbe->e_lmscale[i][1] = sc;
				cbe->e_lmscale[i][2] = sc;
				cbe->e_lmscale[i][3] = 1;
				i++;
			}
			for (; i < MAXRLIGHTMAPS ; i++)
			{
				cbe->e_lmscale[i][0] = 0;
				cbe->e_lmscale[i][1] = 0;
				cbe->e_lmscale[i][2] = 0;
				cbe->e_lmscale[i][3] = 1;
			}
			break;
		}
		#else
		float sc = 1;
		#endif
		if (shaderstate.curentity->model && shaderstate.curentity->model->engineflags & MDLF_NEEDOVERBRIGHT)
			sc = (1<<bound(0, gl_overbright.ival, 2)) * shaderstate.identitylighting;
		else
			sc = shaderstate.identitylighting;
//		sc *= d_lightstylevalue[s]/256.0f;

		Vector4Set(cbe->e_lmscale[i], sc, sc, sc, 1);
	}

	R_FetchPlayerColour(e->topcolour, cbe->e_uppercolour);		cbe->pad4 = 0;
	R_FetchPlayerColour(e->bottomcolour, cbe->e_lowercolour);	cbe->pad5 = 0;
	VectorCopy(e->glowmod, cbe->e_glowmod);						cbe->pad6 = 0;
	if (shaderstate.flags & BEF_FORCECOLOURMOD)
		Vector4Copy(e->shaderRGBAf, cbe->e_colourident);
	else
		Vector4Set(cbe->e_colourident, 1, 1, 1, e->shaderRGBAf[3]);

	VectorCopy(r_refdef.globalfog.colour, cbe->w_fogcolours);
	cbe->w_fogcolours[3] = r_refdef.globalfog.alpha;

	cbe->w_fogdensity = r_refdef.globalfog.density;
	cbe->w_fogdepthbias = r_refdef.globalfog.depthbias;
	Vector2Set(cbe->pad7, 0, 0);

	ndr = (e->flags & RF_DEPTHHACK)?0.333:1;
	if (ndr != shaderstate.depthrange)
	{
		VkViewport viewport;
		shaderstate.depthrange = ndr;

		viewport.x = r_refdef.pxrect.x;
		viewport.y = r_refdef.pxrect.y;
		viewport.width = r_refdef.pxrect.width;
		viewport.height = r_refdef.pxrect.height;
		viewport.minDepth = 0;
		viewport.maxDepth = ndr;
		vkCmdSetViewport(vk.frame->cbuf, 0, 1, &viewport);
	}
}

void VKBE_SubmitBatch(batch_t *batch)
{
	shader_t *shader = batch->shader;
	shaderstate.nummeshes = batch->meshes - batch->firstmesh;
	if (!shaderstate.nummeshes)
		return;
	shaderstate.curbatch = batch;
	shaderstate.batchvbo = batch->vbo;
	shaderstate.meshlist = batch->mesh + batch->firstmesh;
	shaderstate.curshader = shader->remapto;
	if (shaderstate.curentity != batch->ent)
	{
		BE_RotateForEntity(batch->ent, batch->ent->model);
		shaderstate.curtime = r_refdef.time - shaderstate.curentity->shaderTime;
	}
	if (batch->skin)
		shaderstate.curtexnums = batch->skin;
	else if (shader->numdefaulttextures)
		shaderstate.curtexnums = shader->defaulttextures + ((int)(shader->defaulttextures_fps * shaderstate.curtime) % shader->numdefaulttextures);
	else
		shaderstate.curtexnums = shader->defaulttextures;
	shaderstate.flags = batch->flags | shaderstate.forcebeflags;

	BE_DrawMeshChain_Internal();
}

void VKBE_DrawMesh_List(shader_t *shader, int nummeshes, mesh_t **meshlist, vbo_t *vbo, texnums_t *texnums, unsigned int beflags)
{
	shaderstate.curbatch = &shaderstate.dummybatch;
	shaderstate.batchvbo = vbo;
	shaderstate.curshader = shader->remapto;
	if (texnums)
		shaderstate.curtexnums = texnums;
	else if (shader->numdefaulttextures)
		shaderstate.curtexnums = shader->defaulttextures + ((int)(shader->defaulttextures_fps * shaderstate.curtime) % shader->numdefaulttextures);
	else
		shaderstate.curtexnums = shader->defaulttextures;
	shaderstate.meshlist = meshlist;
	shaderstate.nummeshes = nummeshes;
	shaderstate.flags = beflags | shaderstate.forcebeflags;

	BE_DrawMeshChain_Internal();
}

void VKBE_DrawMesh_Single(shader_t *shader, mesh_t *meshchain, vbo_t *vbo, unsigned int beflags)
{
	shaderstate.curbatch = &shaderstate.dummybatch;
	shaderstate.batchvbo = vbo;
	shaderstate.curtime = realtime;
	shaderstate.curshader = shader->remapto;
	if (shader->numdefaulttextures)
		shaderstate.curtexnums = shader->defaulttextures + ((int)(shader->defaulttextures_fps * shaderstate.curtime) % shader->numdefaulttextures);
	else
		shaderstate.curtexnums = shader->defaulttextures;
	shaderstate.meshlist = &meshchain;
	shaderstate.nummeshes = 1;
	shaderstate.flags = beflags | shaderstate.forcebeflags;

	BE_DrawMeshChain_Internal();
}

void VKBE_RT_Destroy(struct vk_rendertarg *targ)
{
	if (targ->framebuffer)
	{
		vkDestroyFramebuffer(vk.device, targ->framebuffer, vkallocationcb);
		VK_DestroyVkTexture(&targ->depth);
		VK_DestroyVkTexture(&targ->colour);
	}
	memset(targ, 0, sizeof(*targ));
}


struct vkbe_rtpurge
{
	struct vk_fencework fw;
	VkFramebuffer framebuffer;
	vk_image_t colour;
	vk_image_t depth;
};
static void VKBE_RT_Purge(void *ptr)
{
	struct vkbe_rtpurge *ctx = ptr;
	vkDestroyFramebuffer(vk.device, ctx->framebuffer, vkallocationcb);
	VK_DestroyVkTexture(&ctx->depth);
	VK_DestroyVkTexture(&ctx->colour);
}
void VKBE_RT_Gen(struct vk_rendertarg *targ, uint32_t width, uint32_t height, qboolean clear)
{
	//sooooo much work...
	VkImageCreateInfo colour_imginfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkImageCreateInfo depth_imginfo;
	struct vkbe_rtpurge *purge;
	static VkClearValue clearvalues[2];

	if (clear)
		targ->restartinfo.renderPass = vk.renderpass[2];
	else
		targ->restartinfo.renderPass = vk.renderpass[1];	//don't care
	targ->restartinfo.clearValueCount = 2;
	targ->depthcleared = true;	//will be once its activated.

	if (targ->width == width && targ->height == height)
		return;	//no work to do.

	if (targ->framebuffer)
	{	//schedule the old one to be destroyed at the end of the current frame. DIE OLD ONE, DIE!
		purge = VK_AtFrameEnd(VKBE_RT_Purge, sizeof(*purge));
		purge->framebuffer = targ->framebuffer; 
		purge->colour = targ->colour;
		purge->depth = targ->depth;
		memset(&targ->colour, 0, sizeof(targ->colour));
		memset(&targ->depth, 0, sizeof(targ->depth));
		targ->framebuffer = VK_NULL_HANDLE;
	}

	targ->q_colour.vkimage = &targ->colour;
	targ->q_depth.vkimage = &targ->depth;
	targ->q_colour.status = TEX_LOADED;
	targ->q_colour.width = width;
	targ->q_colour.height = height;

	targ->width = width;
	targ->height = height;

	if (width == 0 && height == 0)
		return;	//destroyed

	colour_imginfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	colour_imginfo.flags = 0;
	colour_imginfo.imageType = VK_IMAGE_TYPE_2D;
	colour_imginfo.extent.width = width;
	colour_imginfo.extent.height = height;
	colour_imginfo.extent.depth = 1;
	colour_imginfo.mipLevels = 1;
	colour_imginfo.arrayLayers = 1;
	colour_imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
	colour_imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	colour_imginfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	colour_imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	colour_imginfo.queueFamilyIndexCount = 0;
	colour_imginfo.pQueueFamilyIndices = NULL;
	colour_imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkAssert(vkCreateImage(vk.device, &colour_imginfo, vkallocationcb, &targ->colour.image));

	depth_imginfo = colour_imginfo;
	depth_imginfo.format = VK_FORMAT_D32_SFLOAT;
	depth_imginfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	VkAssert(vkCreateImage(vk.device, &depth_imginfo, vkallocationcb, &targ->depth.image));


	{
		VkMemoryRequirements mem_reqs;
		VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		vkGetImageMemoryRequirements(vk.device, targ->colour.image, &mem_reqs);
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &targ->colour.memory));
		VkAssert(vkBindImageMemory(vk.device, targ->colour.image, targ->colour.memory, 0));
	}

	{
		VkMemoryRequirements mem_reqs;
		VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		vkGetImageMemoryRequirements(vk.device, targ->depth.image, &mem_reqs);
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &targ->depth.memory));
		VkAssert(vkBindImageMemory(vk.device, targ->depth.image, targ->depth.memory, 0));
	}

//		set_image_layout(vk.frame->cbuf, targ->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
//		set_image_layout(vk.frame->cbuf, targ->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	{
		VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ivci.components.r = VK_COMPONENT_SWIZZLE_R;
		ivci.components.g = VK_COMPONENT_SWIZZLE_G;
		ivci.components.b = VK_COMPONENT_SWIZZLE_B;
		ivci.components.a = VK_COMPONENT_SWIZZLE_A;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.flags = 0;

		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.format = colour_imginfo.format;
		ivci.image = targ->colour.image;
		VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->colour.view));

		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		ivci.format = depth_imginfo.format;
		ivci.image = targ->depth.image;
		VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->depth.view));
	}

	{
		VkSamplerCreateInfo lmsampinfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		lmsampinfo.minFilter = lmsampinfo.magFilter = VK_FILTER_LINEAR;
		lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		lmsampinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.mipLodBias = 0.0;
		lmsampinfo.anisotropyEnable = VK_FALSE;
		lmsampinfo.maxAnisotropy = 0;
		lmsampinfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		lmsampinfo.minLod = 0;
		lmsampinfo.maxLod = 0;
		lmsampinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		lmsampinfo.unnormalizedCoordinates = VK_FALSE;

		lmsampinfo.compareEnable = VK_FALSE;
		VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &targ->colour.sampler));

		lmsampinfo.compareEnable = VK_TRUE;
		VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &targ->depth.sampler));
	}

	targ->colour.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	targ->depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

	{
		VkFramebufferCreateInfo fbinfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		VkImageView attachments[2] = {targ->colour.view, targ->depth.view};
		fbinfo.flags = 0;
		fbinfo.renderPass = vk.renderpass[2];
		fbinfo.attachmentCount = countof(attachments);
		fbinfo.pAttachments = attachments;
		fbinfo.width = width;
		fbinfo.height = height;
		fbinfo.layers = 1;
		VkAssert(vkCreateFramebuffer(vk.device, &fbinfo, vkallocationcb, &targ->framebuffer));
	}

	targ->restartinfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	targ->restartinfo.pNext = NULL;
	targ->restartinfo.framebuffer = targ->framebuffer;
	targ->restartinfo.renderArea.offset.x = 0;
	targ->restartinfo.renderArea.offset.y = 0;
	targ->restartinfo.renderArea.extent.width = width;
	targ->restartinfo.renderArea.extent.height = height;
	targ->restartinfo.pClearValues = clearvalues;
	clearvalues[1].depthStencil.depth = 1;
}

struct vkbe_rtpurge_cube
{
	struct vk_fencework fw;
	vk_image_t colour;
	vk_image_t depth;
	struct
	{
		VkFramebuffer framebuffer;
		VkImageView iv[2];
	} face[6];
};
static void VKBE_RT_Purge_Cube(void *ptr)
{
	uint32_t f;
	struct vkbe_rtpurge_cube *ctx = ptr;
	for (f = 0; f < 6; f++)
	{
		vkDestroyFramebuffer(vk.device, ctx->face[f].framebuffer, vkallocationcb);
		vkDestroyImageView(vk.device, ctx->face[f].iv[0], vkallocationcb);
		vkDestroyImageView(vk.device, ctx->face[f].iv[1], vkallocationcb);
	}
	VK_DestroyVkTexture(&ctx->depth);
	VK_DestroyVkTexture(&ctx->colour);
}
//generate a cubemap-compatible 2d array, set up 6 render targets that render to their own views
void VKBE_RT_Gen_Cube(struct vk_rendertarg_cube *targ, uint32_t size, qboolean clear)
{
	VkImageCreateInfo colour_imginfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkImageCreateInfo depth_imginfo;
	struct vkbe_rtpurge_cube *purge;
	uint32_t f;
	static VkClearValue clearvalues[2];

	for (f = 0; f < 6; f++)
	{
		if (clear)
			targ->face[f].restartinfo.renderPass = vk.renderpass[2];
		else
			targ->face[f].restartinfo.renderPass = vk.renderpass[1];	//don't care
		targ->face[f].restartinfo.clearValueCount = 2;
	}

	if (targ->size == size)
		return;	//no work to do.

	if (targ->size)
	{	//schedule the old one to be destroyed at the end of the current frame. DIE OLD ONE, DIE!
		purge = VK_AtFrameEnd(VKBE_RT_Purge_Cube, sizeof(*purge));
		for (f = 0; f < 6; f++)
		{
			purge->face[f].framebuffer = targ->face[f].framebuffer;
			targ->face[f].framebuffer = VK_NULL_HANDLE;
			purge->face[f].iv[0] = targ->face[f].colour.view;
			purge->face[f].iv[1] = targ->face[f].depth.view;
			targ->face[f].colour.view = VK_NULL_HANDLE;
			targ->face[f].depth.view = VK_NULL_HANDLE;
		}
		purge->colour = targ->colour;
		purge->depth = targ->depth;
		memset(&targ->colour, 0, sizeof(targ->colour));
		memset(&targ->depth, 0, sizeof(targ->depth));
	}

	targ->size = size;
	if (!size)
		return;

	targ->q_colour.vkimage = &targ->colour;
	targ->q_depth.vkimage = &targ->depth;

	colour_imginfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	colour_imginfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	colour_imginfo.imageType = VK_IMAGE_TYPE_2D;
	colour_imginfo.extent.width = size;
	colour_imginfo.extent.height = size;
	colour_imginfo.mipLevels = 1;
	colour_imginfo.arrayLayers = 6;
	colour_imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
	colour_imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	colour_imginfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	colour_imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	colour_imginfo.queueFamilyIndexCount = 0;
	colour_imginfo.pQueueFamilyIndices = NULL;
	colour_imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkAssert(vkCreateImage(vk.device, &colour_imginfo, vkallocationcb, &targ->colour.image));

	depth_imginfo = colour_imginfo;
	depth_imginfo.format = vk.depthformat;
	depth_imginfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	VkAssert(vkCreateImage(vk.device, &depth_imginfo, vkallocationcb, &targ->depth.image));


	{
		VkMemoryRequirements mem_reqs;
		VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		vkGetImageMemoryRequirements(vk.device, targ->colour.image, &mem_reqs);
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &targ->colour.memory));
		VkAssert(vkBindImageMemory(vk.device, targ->colour.image, targ->colour.memory, 0));
	}

	{
		VkMemoryRequirements mem_reqs;
		VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		vkGetImageMemoryRequirements(vk.device, targ->depth.image, &mem_reqs);
		memAllocInfo.allocationSize = mem_reqs.size;
		memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);
		VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &targ->depth.memory));
		VkAssert(vkBindImageMemory(vk.device, targ->depth.image, targ->depth.memory, 0));
	}

//		set_image_layout(vk.frame->cbuf, targ->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
//		set_image_layout(vk.frame->cbuf, targ->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	//public sampler
	{
		VkSamplerCreateInfo lmsampinfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		lmsampinfo.minFilter = lmsampinfo.magFilter = VK_FILTER_LINEAR;
		lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		lmsampinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		lmsampinfo.mipLodBias = 0.0;
		lmsampinfo.anisotropyEnable = VK_FALSE;
		lmsampinfo.maxAnisotropy = 0;
		lmsampinfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		lmsampinfo.minLod = 0;
		lmsampinfo.maxLod = 0;
		lmsampinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		lmsampinfo.unnormalizedCoordinates = VK_FALSE;

		lmsampinfo.compareEnable = VK_FALSE;
		VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &targ->colour.sampler));

		lmsampinfo.compareEnable = VK_TRUE;
		VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &targ->depth.sampler));
	}

	//public cubemap views
	{
		VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ivci.components.r = VK_COMPONENT_SWIZZLE_R;
		ivci.components.g = VK_COMPONENT_SWIZZLE_G;
		ivci.components.b = VK_COMPONENT_SWIZZLE_B;
		ivci.components.a = VK_COMPONENT_SWIZZLE_A;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 6;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		ivci.flags = 0;

		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.format = colour_imginfo.format;
		ivci.image = targ->colour.image;
		VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->colour.view));

		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		ivci.format = depth_imginfo.format;
		ivci.image = targ->depth.image;
		VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->depth.view));
	}

	for (f = 0; f < 6; f++)
	{
		targ->face[f].width = targ->face[f].height = size;

		//per-face view for the framebuffer
		{
			VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			ivci.components.r = VK_COMPONENT_SWIZZLE_R;
			ivci.components.g = VK_COMPONENT_SWIZZLE_G;
			ivci.components.b = VK_COMPONENT_SWIZZLE_B;
			ivci.components.a = VK_COMPONENT_SWIZZLE_A;
			ivci.subresourceRange.baseMipLevel = 0;
			ivci.subresourceRange.levelCount = 1;
			ivci.subresourceRange.baseArrayLayer = f;
			ivci.subresourceRange.layerCount = 1;
			ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			ivci.flags = 0;

			ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			ivci.format = colour_imginfo.format;
			ivci.image = targ->colour.image;
			VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->face[f].colour.view));

			ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			ivci.format = depth_imginfo.format;
			ivci.image = targ->depth.image;
			VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &targ->face[f].depth.view));
		}

		targ->colour.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		targ->depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

		{
			VkFramebufferCreateInfo fbinfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			VkImageView attachments[2] = {targ->face[f].colour.view, targ->face[f].depth.view};
			fbinfo.flags = 0;
			fbinfo.renderPass = vk.renderpass[2];
			fbinfo.attachmentCount = countof(attachments);
			fbinfo.pAttachments = attachments;
			fbinfo.width = size;
			fbinfo.height = size;
			fbinfo.layers = 1;
			VkAssert(vkCreateFramebuffer(vk.device, &fbinfo, vkallocationcb, &targ->face[f].framebuffer));
		}

		targ->face[f].restartinfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		targ->face[f].restartinfo.pNext = NULL;
		targ->face[f].restartinfo.framebuffer = targ->face[f].framebuffer;
		targ->face[f].restartinfo.renderArea.offset.x = 0;
		targ->face[f].restartinfo.renderArea.offset.y = 0;
		targ->face[f].restartinfo.renderArea.extent.width = size;
		targ->face[f].restartinfo.renderArea.extent.height = size;
		targ->face[f].restartinfo.pClearValues = clearvalues;
	}
	clearvalues[1].depthStencil.depth = 1;
}

void VKBE_RT_Begin(struct vk_rendertarg *targ)
{
	if (vk.rendertarg == targ)
		return;

	if (vk.rendertarg)
		vkCmdEndRenderPass(vk.frame->cbuf);

	r_refdef.pxrect.x = 0;
	r_refdef.pxrect.y = 0;
	r_refdef.pxrect.width = targ->width;
	r_refdef.pxrect.height = targ->height;
	r_refdef.pxrect.maxheight = targ->height;

	vid.fbpwidth = targ->width;
	vid.fbpheight = targ->height;

	vkCmdBeginRenderPass(vk.frame->cbuf, &targ->restartinfo, VK_SUBPASS_CONTENTS_INLINE);
	//future reuse shouldn't clear stuff
	if (targ->restartinfo.clearValueCount)
	{
		targ->depthcleared = true;
		targ->restartinfo.renderPass = vk.renderpass[0];
		targ->restartinfo.clearValueCount = 0;
	}
	vk.rendertarg = targ;

	{
		VkRect2D wrekt;
		VkViewport viewport;
		viewport.x = r_refdef.pxrect.x;
		viewport.y = r_refdef.pxrect.y;
		viewport.width = r_refdef.pxrect.width;
		viewport.height = r_refdef.pxrect.height;
		viewport.minDepth = 0;
		viewport.maxDepth = shaderstate.depthrange;
		vkCmdSetViewport(vk.frame->cbuf, 0, 1, &viewport);
		wrekt.offset.x = viewport.x;
		wrekt.offset.y = viewport.y;
		wrekt.extent.width = viewport.width;
		wrekt.extent.height = viewport.height;
		vkCmdSetScissor(vk.frame->cbuf, 0, 1, &wrekt);
	}
}

static qboolean BE_GenerateRefraction(batch_t *batch, shader_t *bs)
{
	float oldil;
	int oldbem;
	struct vk_rendertarg *targ;
	//these flags require rendering some view as an fbo
	if (r_refdef.recurse)
		return false;
	if (shaderstate.mode != BEM_STANDARD && shaderstate.mode != BEM_DEPTHDARK)
		return false;
	oldbem = shaderstate.mode;
	oldil = shaderstate.identitylighting;
	targ = vk.rendertarg;

	if (bs->flags & SHADER_HASREFLECT)
	{
		vrect_t orect = r_refdef.vrect;
		pxrect_t oprect = r_refdef.pxrect;

		r_refdef.vrect.x = 0;
		r_refdef.vrect.y = 0;
		r_refdef.vrect.width = vid.fbvwidth/2;
		r_refdef.vrect.height = vid.fbvheight/2;
		VKBE_RT_Gen(&shaderstate.rt_reflection, vid.fbpwidth/2, vid.fbpheight/2, false);
		VKBE_RT_Begin(&shaderstate.rt_reflection);
		R_DrawPortal(batch, cl.worldmodel->batches, NULL, 1);
		r_refdef.vrect = orect;
		r_refdef.pxrect = oprect;
	}
	if (bs->flags & (SHADER_HASREFRACT|SHADER_HASREFRACTDEPTH))
	{
		extern cvar_t r_refract_fbo;
		if (r_refract_fbo.ival || (bs->flags & SHADER_HASREFRACTDEPTH))
		{
			vrect_t ovrect = r_refdef.vrect;
			pxrect_t oprect = r_refdef.pxrect;

			r_refdef.vrect.x = 0;
			r_refdef.vrect.y = 0;
			r_refdef.vrect.width = vid.fbvwidth/2;
			r_refdef.vrect.height = vid.fbvheight/2;
			VKBE_RT_Gen(&shaderstate.rt_refraction, vid.fbpwidth/2, vid.fbpheight/2, false);
			VKBE_RT_Begin(&shaderstate.rt_refraction);
			R_DrawPortal(batch, cl.worldmodel->batches, NULL, ((bs->flags & SHADER_HASREFRACTDEPTH)?3:2));	//fixme
			r_refdef.vrect = ovrect;
			r_refdef.pxrect = oprect;

			shaderstate.tex_refraction = &shaderstate.rt_refraction.q_colour;
			VKBE_RT_Begin(targ);
		}
		else
		{
			VKBE_RT_Begin(targ);
			R_DrawPortal(batch, cl.worldmodel->batches, NULL, 3);
			T_Gen_CurrentRender();
			shaderstate.tex_refraction = shaderstate.tex_currentrender;
		}
	}
	/*
	if (bs->flags & SHADER_HASRIPPLEMAP)
	{
		vrect_t orect = r_refdef.vrect;
		pxrect_t oprect = r_refdef.pxrect;
		r_refdef.vrect.x = 0;
		r_refdef.vrect.y = 0;
		r_refdef.vrect.width = vid.fbvwidth/2;
		r_refdef.vrect.height = vid.fbvheight/2;
		r_refdef.pxrect.x = 0;
		r_refdef.pxrect.y = 0;
		r_refdef.pxrect.width = vid.fbpwidth/2;
		r_refdef.pxrect.height = vid.fbpheight/2;

		if (!shaderstate.tex_ripplemap)
		{
			//FIXME: can we use RGB8 instead?
			shaderstate.tex_ripplemap = Image_CreateTexture("***tex_ripplemap***", NULL, 0);
			if (!shaderstate.tex_ripplemap->num)
				qglGenTextures(1, &shaderstate.tex_ripplemap->num);
		}
		if (shaderstate.tex_ripplemap->width != r_refdef.pxrect.width || shaderstate.tex_ripplemap->height != r_refdef.pxrect.height)
		{
			shaderstate.tex_ripplemap->width = r_refdef.pxrect.width;
			shaderstate.tex_ripplemap->height = r_refdef.pxrect.height;
			GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_ripplemap);
			qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		oldfbo = GLBE_FBO_Update(&shaderstate.fbo_reflectrefrac, 0, &shaderstate.tex_ripplemap, 1, r_nulltex, r_refdef.pxrect.width, r_refdef.pxrect.height, 0);
		r_refdef.pxrect.maxheight = shaderstate.fbo_reflectrefrac.rb_size[1];
		GL_ViewportUpdate();

		qglClearColor(0, 0, 0, 1);
		qglClear(GL_COLOR_BUFFER_BIT);

		r_refdef.vrect.x = 0;
		r_refdef.vrect.y = 0;
		r_refdef.vrect.width = vid.fbvwidth;
		r_refdef.vrect.height = vid.fbvheight;
		BE_RT_Begin(&shaderstate.rt_refraction, vid.fbpwidth, vid.fbpheight);

		r_refdef.recurse+=1; //paranoid, should stop potential infinite loops
		GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_RIPPLE, SHADER_SORT_RIPPLE);
		r_refdef.recurse-=1;

		r_refdef.vrect = orect;
		r_refdef.pxrect = oprect;
		BE_RT_End();
	}
	*/
	VKBE_RT_Begin(targ);
	VKBE_SelectMode(oldbem);
	shaderstate.identitylighting = oldil;

	return true;
}

static void BE_SubmitMeshesSortList(batch_t *sortlist)
{
	batch_t *batch;
	for (batch = sortlist; batch; batch = batch->next)
	{
		if (batch->meshes == batch->firstmesh)
			continue;

		if (batch->buildmeshes)
			batch->buildmeshes(batch);

		if (batch->shader->flags & SHADER_NODLIGHT)
			if (shaderstate.mode == BEM_LIGHT)
				continue;

		if (batch->shader->flags & SHADER_SKY)
		{
			if (!batch->shader->prog)
			{
				if (shaderstate.mode == BEM_STANDARD)
					R_DrawSkyChain (batch);
				continue;
			}
		}

		if ((batch->shader->flags & (SHADER_HASREFLECT | SHADER_HASREFRACT | SHADER_HASRIPPLEMAP)) && shaderstate.mode != BEM_WIREFRAME)
			if (!BE_GenerateRefraction(batch, batch->shader))
				continue;

		VKBE_SubmitBatch(batch);
	}
}


/*generates a new modelview matrix, as well as vpn vectors*/
static void R_MirrorMatrix(plane_t *plane)
{
	float mirror[16];
	float view[16];
	float result[16];

	vec3_t pnorm;
	VectorNegate(plane->normal, pnorm);

	mirror[0] = 1-2*pnorm[0]*pnorm[0];
	mirror[1] = -2*pnorm[0]*pnorm[1];
	mirror[2] = -2*pnorm[0]*pnorm[2];
	mirror[3] = 0;

	mirror[4] = -2*pnorm[1]*pnorm[0];
	mirror[5] = 1-2*pnorm[1]*pnorm[1];
	mirror[6] = -2*pnorm[1]*pnorm[2] ;
	mirror[7] = 0;

	mirror[8]  = -2*pnorm[2]*pnorm[0];
	mirror[9]  = -2*pnorm[2]*pnorm[1];
	mirror[10] = 1-2*pnorm[2]*pnorm[2];
	mirror[11] = 0;

	mirror[12] = -2*pnorm[0]*plane->dist;
	mirror[13] = -2*pnorm[1]*plane->dist;
	mirror[14] = -2*pnorm[2]*plane->dist;
	mirror[15] = 1;

	view[0] = vpn[0];
	view[1] = vpn[1];
	view[2] = vpn[2];
	view[3] = 0;

	view[4] = -vright[0];
	view[5] = -vright[1];
	view[6] = -vright[2];
	view[7] = 0;

	view[8]  = vup[0];
	view[9]  = vup[1];
	view[10] = vup[2];
	view[11] = 0;

	view[12] = r_refdef.vieworg[0];
	view[13] = r_refdef.vieworg[1];
	view[14] = r_refdef.vieworg[2];
	view[15] = 1;

	VectorMA(r_refdef.vieworg, 0.25, plane->normal, r_refdef.pvsorigin);

	Matrix4_Multiply(mirror, view, result);

	vpn[0] = result[0];
	vpn[1] = result[1];
	vpn[2] = result[2];

	vright[0] = -result[4];
	vright[1] = -result[5];
	vright[2] = -result[6];

	vup[0] = result[8];
	vup[1] = result[9];
	vup[2] = result[10];

	r_refdef.vieworg[0] = result[12];
	r_refdef.vieworg[1] = result[13];
	r_refdef.vieworg[2] = result[14];
}
static entity_t *R_NearestPortal(plane_t *plane)
{
	int i;
	entity_t *best = NULL;
	float dist, bestd = 0;
	//for q3-compat, portals on world scan for a visedict to use for their view.
	for (i = 0; i < cl_numvisedicts; i++)
	{
		if (cl_visedicts[i].rtype == RT_PORTALSURFACE)
		{
			dist = DotProduct(cl_visedicts[i].origin, plane->normal)-plane->dist;
			dist = fabs(dist);
			if (dist < 64 && (!best || dist < bestd))
				best = &cl_visedicts[i];
		}
	}
	return best;
}

static void TransformCoord(vec3_t in, vec3_t planea[3], vec3_t planeo, vec3_t viewa[3], vec3_t viewo, vec3_t result)
{
	int		i;
	vec3_t	local;
	vec3_t	transformed;
	float	d;

	local[0] = in[0] - planeo[0];
	local[1] = in[1] - planeo[1];
	local[2] = in[2] - planeo[2];

	VectorClear(transformed);
	for ( i = 0 ; i < 3 ; i++ )
	{
		d = DotProduct(local, planea[i]);
		VectorMA(transformed, d, viewa[i], transformed);
	}

	result[0] = transformed[0] + viewo[0];
	result[1] = transformed[1] + viewo[1];
	result[2] = transformed[2] + viewo[2];
}
static void TransformDir(vec3_t in, vec3_t planea[3], vec3_t viewa[3], vec3_t result)
{
	int		i;
	float	d;
	vec3_t tmp;

	VectorCopy(in, tmp);

	VectorClear(result);
	for ( i = 0 ; i < 3 ; i++ )
	{
		d = DotProduct(tmp, planea[i]);
		VectorMA(result, d, viewa[i], result);
	}
}

void R_ObliqueNearClip(float *viewmat, mplane_t *wplane);
void CL_DrawDebugPlane(float *normal, float dist, float r, float g, float b, qboolean enqueue);
static void R_DrawPortal(batch_t *batch, batch_t **blist, batch_t *depthmasklist[2], int portaltype)
{
	entity_t *view;
	plane_t plane, oplane;
	float vmat[16];
	refdef_t oldrefdef;
	vec3_t r;
	int i;
	mesh_t *mesh = batch->mesh[batch->firstmesh];
	pvsbuffer_t newvis;
	float ivmat[16], trmat[16];

	if (r_refdef.recurse >= R_MAX_RECURSE-1)
		return;

	if (!mesh->xyz_array)
		return;

	if (!mesh->normals_array)
	{
		VectorSet(plane.normal, 0, 0, 1);
	}
	else
	{
		VectorCopy(mesh->normals_array[0], plane.normal);
	}

	if (batch->ent == &r_worldentity)
	{
		plane.dist = DotProduct(mesh->xyz_array[0], plane.normal);
	}
	else
	{
		vec3_t point;
		VectorCopy(plane.normal, oplane.normal);
		//rotate the surface normal around its entity's matrix
		plane.normal[0] = oplane.normal[0]*batch->ent->axis[0][0] + oplane.normal[1]*batch->ent->axis[1][0] + oplane.normal[2]*batch->ent->axis[2][0];
		plane.normal[1] = oplane.normal[0]*batch->ent->axis[0][1] + oplane.normal[1]*batch->ent->axis[1][1] + oplane.normal[2]*batch->ent->axis[2][1];
		plane.normal[2] = oplane.normal[0]*batch->ent->axis[0][2] + oplane.normal[1]*batch->ent->axis[1][2] + oplane.normal[2]*batch->ent->axis[2][2];

		//rotate some point on the mesh around its entity's matrix
		point[0] = mesh->xyz_array[0][0]*batch->ent->axis[0][0] + mesh->xyz_array[0][1]*batch->ent->axis[1][0] + mesh->xyz_array[0][2]*batch->ent->axis[2][0] + batch->ent->origin[0];
		point[1] = mesh->xyz_array[0][0]*batch->ent->axis[0][1] + mesh->xyz_array[0][1]*batch->ent->axis[1][1] + mesh->xyz_array[0][2]*batch->ent->axis[2][1] + batch->ent->origin[1];
		point[2] = mesh->xyz_array[0][0]*batch->ent->axis[0][2] + mesh->xyz_array[0][1]*batch->ent->axis[1][2] + mesh->xyz_array[0][2]*batch->ent->axis[2][2] + batch->ent->origin[2];

		//now we can figure out the plane dist
		plane.dist = DotProduct(point, plane.normal);
	}

	//if we're too far away from the surface, don't draw anything
	if (batch->shader->flags & SHADER_AGEN_PORTAL)
	{
		/*there's a portal alpha blend on that surface, that fades out after this distance*/
		if (DotProduct(r_refdef.vieworg, plane.normal)-plane.dist > batch->shader->portaldist)
			return;
	}
	//if we're behind it, then also don't draw anything. for our purposes, behind is when the entire near clipplane is behind.
	if (DotProduct(r_refdef.vieworg, plane.normal)-plane.dist < -r_refdef.mindist)
		return;

	TRACE(("R_DrawPortal: portal type %i\n", portaltype));

	oldrefdef = r_refdef;
	r_refdef.recurse+=1;

	r_refdef.externalview = true;

	switch(portaltype)
	{
	case 1: /*fbo explicit mirror (fucked depth, working clip plane)*/
		//fixme: pvs is surely wrong?
//		r_refdef.flipcull ^= SHADER_CULL_FLIP;
		R_MirrorMatrix(&plane);
		Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

		VectorCopy(mesh->xyz_array[0], r_refdef.pvsorigin);
		for (i = 1; i < mesh->numvertexes; i++)
			VectorAdd(r_refdef.pvsorigin, mesh->xyz_array[i], r_refdef.pvsorigin);
		VectorScale(r_refdef.pvsorigin, 1.0/mesh->numvertexes, r_refdef.pvsorigin);
		break;
	
	case 2:	/*fbo refraction (fucked depth, working clip plane)*/
	case 3:	/*screen copy refraction (screen depth, fucked clip planes)*/
		/*refraction image (same view, just with things culled*/
		r_refdef.externalview = oldrefdef.externalview;
		VectorNegate(plane.normal, plane.normal);
		plane.dist = -plane.dist;

		//use the player's origin for r_viewleaf, because there's not much we can do anyway*/
		VectorCopy(r_origin, r_refdef.pvsorigin);

		if (cl.worldmodel && cl.worldmodel->funcs.ClusterPVS && !r_novis.ival)
		{
			int clust, i, j;
			float d;
			vec3_t point;
			r_refdef.forcevis = true;
			r_refdef.forcedvis = NULL;
			newvis.buffer = alloca(newvis.buffersize=cl.worldmodel->pvsbytes);
			for (i = batch->firstmesh; i < batch->meshes; i++)
			{
				mesh = batch->mesh[i];
				VectorClear(point);
				for (j = 0; j < mesh->numvertexes; j++)
					VectorAdd(point, mesh->xyz_array[j], point);
				VectorScale(point, 1.0f/mesh->numvertexes, point);
				d = DotProduct(point, plane.normal) - plane.dist;
				d += 0.1;	//an epsilon on the far side
				VectorMA(point, d, plane.normal, point);

				clust = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, point);
				if (i == batch->firstmesh)
					r_refdef.forcedvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clust, &newvis, PVM_REPLACE);
				else
					r_refdef.forcedvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clust, &newvis, PVM_MERGE);
			}
//			memset(newvis, 0xff, pvsbytes);
		}
		Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);
		break;

	case 0:		/*q3 portal*/
	default:
#ifdef CSQC_DAT
		if (CSQC_SetupToRenderPortal(batch->ent->keynum))
		{
			oplane = plane;

			//transform the old surface plane into the new view matrix
			Matrix4_Invert(r_refdef.m_view, ivmat);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);
			Matrix4_Multiply(ivmat, vmat, trmat);
			plane.normal[0] = -(oplane.normal[0] * trmat[0] + oplane.normal[1] * trmat[1] + oplane.normal[2] * trmat[2]);
			plane.normal[1] = -(oplane.normal[0] * trmat[4] + oplane.normal[1] * trmat[5] + oplane.normal[2] * trmat[6]);
			plane.normal[2] = -(oplane.normal[0] * trmat[8] + oplane.normal[1] * trmat[9] + oplane.normal[2] * trmat[10]);
			plane.dist = -oplane.dist + trmat[12]*oplane.normal[0] + trmat[13]*oplane.normal[1] + trmat[14]*oplane.normal[2];

			if (Cvar_Get("temp_useplaneclip", "1", 0, "temp")->ival)
				portaltype = 1;	//make sure the near clipplane is used.
		}
		else
#endif
			if (!(view = R_NearestPortal(&plane)) || VectorCompare(view->origin, view->oldorigin))
		{
			//a portal with no portal entity, or a portal rentity with an origin equal to its oldorigin, is a mirror.
//			r_refdef.flipcull ^= SHADER_CULL_FLIP;
			R_MirrorMatrix(&plane);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

			VectorCopy(mesh->xyz_array[0], r_refdef.pvsorigin);
			for (i = 1; i < mesh->numvertexes; i++)
				VectorAdd(r_refdef.pvsorigin, mesh->xyz_array[i], r_refdef.pvsorigin);
			VectorScale(r_refdef.pvsorigin, 1.0/mesh->numvertexes, r_refdef.pvsorigin);

			portaltype = 1;
		}
		else
		{
			float d;
			vec3_t paxis[3], porigin, vaxis[3], vorg;
			void PerpendicularVector( vec3_t dst, const vec3_t src );

			oplane = plane;

			/*calculate where the surface is meant to be*/
			VectorCopy(mesh->normals_array[0], paxis[0]);
			PerpendicularVector(paxis[1], paxis[0]);
			CrossProduct(paxis[0], paxis[1], paxis[2]);
			d = DotProduct(view->origin, plane.normal) - plane.dist;
			VectorMA(view->origin, -d, paxis[0], porigin);

			/*grab the camera origin*/
			VectorNegate(view->axis[0], vaxis[0]);
			VectorNegate(view->axis[1], vaxis[1]);
			VectorCopy(view->axis[2], vaxis[2]);
			VectorCopy(view->oldorigin, vorg);

			VectorCopy(vorg, r_refdef.pvsorigin);

			/*rotate it a bit*/
			if (view->framestate.g[FS_REG].frame[1])	//oldframe
			{
				if (view->framestate.g[FS_REG].frame[0])	//newframe
					d = realtime * view->framestate.g[FS_REG].frame[0];	//newframe
				else
					d = view->skinnum + sin(realtime)*4;
			}
			else
				d = view->skinnum;

			if (d)
			{
				vec3_t rdir;
				VectorCopy(vaxis[1], rdir);
				RotatePointAroundVector(vaxis[1], vaxis[0], rdir, d);
				CrossProduct(vaxis[0], vaxis[1], vaxis[2]);
			}

			TransformCoord(oldrefdef.vieworg, paxis, porigin, vaxis, vorg, r_refdef.vieworg);
			TransformDir(vpn, paxis, vaxis, vpn);
			TransformDir(vright, paxis, vaxis, vright);
			TransformDir(vup, paxis, vaxis, vup);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

			//transform the old surface plane into the new view matrix
			if (Matrix4_Invert(r_refdef.m_view, ivmat))
			{
				Matrix4_Multiply(ivmat, vmat, trmat);
				plane.normal[0] = -(oplane.normal[0] * trmat[0] + oplane.normal[1] * trmat[1] + oplane.normal[2] * trmat[2]);
				plane.normal[1] = -(oplane.normal[0] * trmat[4] + oplane.normal[1] * trmat[5] + oplane.normal[2] * trmat[6]);
				plane.normal[2] = -(oplane.normal[0] * trmat[8] + oplane.normal[1] * trmat[9] + oplane.normal[2] * trmat[10]);
				plane.dist = -oplane.dist + trmat[12]*oplane.normal[0] + trmat[13]*oplane.normal[1] + trmat[14]*oplane.normal[2];
				portaltype = 1;
			}
		}
		break;
	}

	/*FIXME: can we get away with stenciling the screen?*/
	/*Add to frustum culling instead of clip planes?*/
/*	if (qglClipPlane && portaltype)
	{
		GLdouble glplane[4];
		glplane[0] = plane.normal[0];
		glplane[1] = plane.normal[1];
		glplane[2] = plane.normal[2];
		glplane[3] = plane.dist;
		qglClipPlane(GL_CLIP_PLANE0, glplane);
		qglEnable(GL_CLIP_PLANE0);
	}
*/	//fixme: we can probably scissor a smaller frusum
	R_SetFrustum (r_refdef.m_projection, vmat);
	if (r_refdef.frustum_numplanes < MAXFRUSTUMPLANES)
	{
		extern int SignbitsForPlane (mplane_t *out);
		mplane_t fp;
		VectorCopy(plane.normal, fp.normal);
		fp.dist = plane.dist;

//		if (DotProduct(fp.normal, vpn) < 0)
//		{
//			VectorNegate(fp.normal, fp.normal);
//			fp.dist *= -1;
//		}

		fp.type = PLANE_ANYZ;
		fp.signbits = SignbitsForPlane (&fp);

		if (portaltype == 1 || portaltype == 2)
			R_ObliqueNearClip(vmat, &fp);

		//our own culling should be an epsilon forwards so we don't still draw things behind the line due to precision issues.
		fp.dist += 0.01;
		r_refdef.frustum[r_refdef.frustum_numplanes++] = fp;
	}

	//force culling to update to match the new front face.
//	memcpy(r_refdef.m_view, vmat, sizeof(float)*16);
#if 0
	if (depthmasklist)
	{
		/*draw already-drawn portals as depth-only, to ensure that their contents are not harmed*/
		/*we can only do this AFTER the oblique perspective matrix is calculated, to avoid depth inconsistancies, while we still have the old view matrix*/
		int i;
		batch_t *dmask = NULL;
		//portals to mask are relative to the old view still.
		GLBE_SelectEntity(&r_worldentity);
		currententity = NULL;
		if (gl_config.arb_depth_clamp)
			qglEnable(GL_DEPTH_CLAMP_ARB);	//ignore the near clip plane(ish), this means nearer portals can still mask further ones.
		GL_ForceDepthWritable();
		GLBE_SelectMode(BEM_DEPTHONLY);
		for (i = 0; i < 2; i++)
		{
			for (dmask = depthmasklist[i]; dmask; dmask = dmask->next)
			{
				if (dmask == batch)
					continue;
				if (dmask->meshes == dmask->firstmesh)
					continue;
				GLBE_SubmitBatch(dmask);
			}
		}
		GLBE_SelectMode(BEM_STANDARD);
		if (gl_config.arb_depth_clamp)
			qglDisable(GL_DEPTH_CLAMP_ARB);

		currententity = NULL;
	}
#endif

	currententity = NULL;

	//now determine the stuff the backend will use.
	memcpy(r_refdef.m_view, vmat, sizeof(float)*16);
	VectorAngles(vpn, vup, r_refdef.viewangles, false);
	VectorCopy(r_refdef.vieworg, r_origin);

	//determine r_refdef.flipcull & SHADER_CULL_FLIP based upon whether right is right or not.
	CrossProduct(vpn, vup, r);
	if (DotProduct(r, vright) < 0)
		r_refdef.flipcull |= SHADER_CULL_FLIP;
	else
		r_refdef.flipcull &= ~SHADER_CULL_FLIP;
	if (r_refdef.m_projection[5]<0)
		r_refdef.flipcull ^= SHADER_CULL_FLIP;

	VKBE_SelectEntity(&r_worldentity);

	Surf_SetupFrame();
	Surf_DrawWorld();
	//FIXME: just call Surf_DrawWorld instead?
//	R_RenderScene();

#if 0
	if (r_portaldrawplanes.ival)
	{
		//the front of the plane should generally point away from the camera, and will be drawn in bright green. woo
		CL_DrawDebugPlane(plane.normal, plane.dist+0.01, 0.0, 0.5, 0.0, false);
		CL_DrawDebugPlane(plane.normal, plane.dist-0.01, 0.0, 0.5, 0.0, false);
		//the back of the plane points towards the camera, and will be drawn in blue, for the luls
		VectorNegate(plane.normal, plane.normal);
		plane.dist *= -1;
		CL_DrawDebugPlane(plane.normal, plane.dist+0.01, 0.0, 0.0, 0.2, false);
		CL_DrawDebugPlane(plane.normal, plane.dist-0.01, 0.0, 0.0, 0.2, false);
	}
#endif


	r_refdef = oldrefdef;

	/*broken stuff*/
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);
	VectorCopy (r_refdef.vieworg, r_origin);

	VKBE_SelectEntity(&r_worldentity);

	TRACE(("GLR_DrawPortal: portal drawn\n"));

	currententity = NULL;
}

static void BE_SubmitMeshesPortals(batch_t **worldlist, batch_t *dynamiclist)
{
	batch_t *batch, *old;
	int i;
	/*attempt to draw portal shaders*/
	if (shaderstate.mode == BEM_STANDARD)
	{
		for (i = 0; i < 2; i++)
		{
			for (batch = i?dynamiclist:worldlist[SHADER_SORT_PORTAL]; batch; batch = batch->next)
			{
				if (batch->meshes == batch->firstmesh)
					continue;

				if (batch->buildmeshes)
					batch->buildmeshes(batch);

				/*draw already-drawn portals as depth-only, to ensure that their contents are not harmed*/
				VKBE_SelectMode(BEM_DEPTHONLY);
				for (old = worldlist[SHADER_SORT_PORTAL]; old && old != batch; old = old->next)
				{
					if (old->meshes == old->firstmesh)
						continue;
					VKBE_SubmitBatch(old);
				}
				if (!old)
				{
					for (old = dynamiclist; old != batch; old = old->next)
					{
						if (old->meshes == old->firstmesh)
							continue;
						VKBE_SubmitBatch(old);
					}
				}
				VKBE_SelectMode(BEM_STANDARD);

				R_DrawPortal(batch, worldlist, NULL, 0);

				{
					VkClearAttachment clr;
					VkClearRect rect;
					clr.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					clr.clearValue.depthStencil.depth = 1;
					clr.clearValue.depthStencil.stencil = 0;
					clr.colorAttachment = 1;
					rect.rect.offset.x = r_refdef.pxrect.x;
					rect.rect.offset.y = r_refdef.pxrect.y;
					rect.rect.extent.width = r_refdef.pxrect.width;
					rect.rect.extent.height = r_refdef.pxrect.height;
					rect.layerCount = 1;
					rect.baseArrayLayer = 0;
					vkCmdClearAttachments(vk.frame->cbuf, 1, &clr, 1, &rect);
				}
				VKBE_SelectMode(BEM_DEPTHONLY);
				VKBE_SubmitBatch(batch);
				VKBE_SelectMode(BEM_STANDARD);
			}
		}
	}
}

void VKBE_SubmitMeshes (batch_t **worldbatches, batch_t **blist, int first, int stop)
{
	int i;

	for (i = first; i < stop; i++)
	{
		if (worldbatches)
		{
			if (i == SHADER_SORT_PORTAL  && !r_refdef.recurse)
				BE_SubmitMeshesPortals(worldbatches, blist[i]);

			BE_SubmitMeshesSortList(worldbatches[i]);
		}
		BE_SubmitMeshesSortList(blist[i]);
	}
}

#ifdef RTLIGHTS
//FIXME: needs context for threading
void VKBE_BaseEntTextures(void)
{
	batch_t *batches[SHADER_SORT_COUNT];
	BE_GenModelBatches(batches, shaderstate.curdlight, shaderstate.mode);
	VKBE_SubmitMeshes(NULL, batches, SHADER_SORT_PORTAL, SHADER_SORT_SEETHROUGH+1);
	VKBE_SelectEntity(&r_worldentity);
}

struct vk_shadowbuffer
{
	qboolean isstatic;

	VkBuffer vbuffer;
	VkDeviceSize voffset;
	VkDeviceMemory vmemory;
	unsigned int numverts;

	VkBuffer ibuffer;
	VkDeviceSize ioffset;
	VkDeviceMemory imemory;
	unsigned int numindicies;
};
//FIXME: needs context for threading
struct vk_shadowbuffer *VKBE_GenerateShadowBuffer(vecV_t *verts, int numverts, index_t *indicies, int numindicies, qboolean istemp)
{
	static struct vk_shadowbuffer tempbuf;
	if (!numverts || !numindicies)
		return NULL;
	if (istemp)
	{
		struct vk_shadowbuffer *buf = &tempbuf;
		void *map;

		map = VKBE_AllocateBufferSpace(DB_VBO, sizeof(*verts)*numverts, &buf->vbuffer, &buf->voffset);
		memcpy(map, verts, sizeof(*verts)*numverts);
		buf->vmemory = VK_NULL_HANDLE;
		buf->numverts = numverts;

		map = VKBE_AllocateBufferSpace(DB_EBO, sizeof(*indicies)*numindicies, &buf->ibuffer, &buf->ioffset);
		memcpy(map, indicies, sizeof(*indicies)*numindicies);
		buf->imemory = VK_NULL_HANDLE;
		buf->numindicies = numindicies;
		return buf;
	}
	else
	{
		//FIXME: these buffers should really be some subsection of a larger buffer
		struct vk_shadowbuffer *buf = BZ_Malloc(sizeof(*buf));
		struct stagingbuf vbuf;
		void *map;
		buf->isstatic = true;

		map = VKBE_CreateStagingBuffer(&vbuf, sizeof(*verts) * numverts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
		memcpy(map, verts, sizeof(*verts) * numverts);
		buf->vbuffer = VKBE_FinishStaging(&vbuf, &buf->vmemory);
		buf->voffset = 0;
		buf->numverts = numverts;

		map = VKBE_CreateStagingBuffer(&vbuf, sizeof(*indicies) * numindicies, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
		memcpy(map, indicies, sizeof(*indicies) * numindicies);
		buf->ibuffer = VKBE_FinishStaging(&vbuf, &buf->imemory);
		buf->ioffset = 0;
		buf->numindicies = numindicies;
		return buf;
	}
}
struct vk_shadowbuffer_destroy
{
	struct vk_fencework fw;
	struct vk_shadowbuffer buf;
};
static void VKBE_DestroyShadowBuffer_Delayed(void *ctx)
{
	struct vk_shadowbuffer_destroy *d = ctx;
	struct vk_shadowbuffer *buf = &d->buf;
	vkDestroyBuffer(vk.device, buf->vbuffer, vkallocationcb);
	vkDestroyBuffer(vk.device, buf->ibuffer, vkallocationcb);
	vkFreeMemory(vk.device, buf->vmemory, vkallocationcb);
	vkFreeMemory(vk.device, buf->imemory, vkallocationcb);
}
void VKBE_DestroyShadowBuffer(struct vk_shadowbuffer *buf)
{
	if (buf && buf->isstatic)
	{
		struct vk_shadowbuffer_destroy *ctx = VK_AtFrameEnd(VKBE_DestroyShadowBuffer_Delayed, sizeof(*ctx));
		ctx->buf = *buf;
		Z_Free(buf);
	}
}

//draws all depth-only surfaces from the perspective of the light.
//FIXME: needs context for threading
void VKBE_RenderShadowBuffer(struct vk_shadowbuffer *buf)
{
	shader_t *depthonlyshader;
	if (!buf)
		return;

	depthonlyshader = R_RegisterShader("depthonly", SUF_NONE,
				"{\n"
					"program depthonly\n"
					"{\n"
						"depthwrite\n"
						"maskcolor\n"
					"}\n"
				"}\n"
			);

	vkCmdBindVertexBuffers(vk.frame->cbuf, 0, 1, &buf->vbuffer, &buf->voffset);
	vkCmdBindIndexBuffer(vk.frame->cbuf, buf->ibuffer, buf->ioffset, VK_INDEX_TYPE);
	if (BE_SetupMeshProgram(depthonlyshader->passes[0].prog, depthonlyshader->passes, 0, buf->numindicies))
		vkCmdDrawIndexed(vk.frame->cbuf, buf->numindicies, 1, 0, 0, 0);
}


static void VK_TerminateShadowMap(void)
{
	struct shadowmaps_s *shad;
	unsigned int sbuf, i;

	if (vk.shadow_renderpass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(vk.device, vk.shadow_renderpass, vkallocationcb);
		vk.shadow_renderpass = VK_NULL_HANDLE;
	}

	for (sbuf = 0; sbuf < countof(shaderstate.shadow); sbuf++)
	{
		shad = &shaderstate.shadow[sbuf];
		if (!shad->image)
			continue;

		for (i = 0; i < countof(shad->buf); i++)
		{
			vkDestroyImageView(vk.device, shad->buf[i].vimage.view, vkallocationcb);
			vkDestroySampler(vk.device, shad->buf[i].vimage.sampler, vkallocationcb);
			vkDestroyFramebuffer(vk.device, shad->buf[i].framebuffer, vkallocationcb);
		}
		vkDestroyImage(vk.device, shad->image, vkallocationcb);
		vkFreeMemory(vk.device, shad->memory, vkallocationcb);

		shad->width = 0;
		shad->height = 0;
	}
}

qboolean VKBE_BeginShadowmap(qboolean isspot, uint32_t width, uint32_t height)
{
	struct shadowmaps_s *shad = &shaderstate.shadow[isspot];
	unsigned int sbuf;

//	const qboolean altqueue = false;

//	if (!altqueue)
//		vkCmdEndRenderPass(vk.frame->cbuf);

	if (shad->width != width || shad->height != height)
	{
		//actually, this will really only happen once per.
		//so we can be lazy and not free here... check out validation/leak warnings if this changes...

		unsigned int i;
		VkFramebufferCreateInfo fbinfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		VkImageCreateInfo imginfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		imginfo.format = VK_FORMAT_D32_SFLOAT;
		imginfo.flags = 0;
		imginfo.imageType = VK_IMAGE_TYPE_2D;
		imginfo.extent.width = width;
		imginfo.extent.height = height;
		imginfo.extent.depth = 1;
		imginfo.mipLevels = 1;
		imginfo.arrayLayers = countof(shad->buf);
		imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imginfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
		imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imginfo.queueFamilyIndexCount = 0;
		imginfo.pQueueFamilyIndices = NULL;
		imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkAssert(vkCreateImage(vk.device, &imginfo, vkallocationcb, &shad->image));

		{
			VkMemoryRequirements mem_reqs;
			VkMemoryAllocateInfo memAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			vkGetImageMemoryRequirements(vk.device, shad->image, &mem_reqs);
			memAllocInfo.allocationSize = mem_reqs.size;
			memAllocInfo.memoryTypeIndex = vk_find_memory_require(mem_reqs.memoryTypeBits, 0);
			VkAssert(vkAllocateMemory(vk.device, &memAllocInfo, vkallocationcb, &shad->memory));
			VkAssert(vkBindImageMemory(vk.device, shad->image, shad->memory, 0));
		}

		if (vk.shadow_renderpass == VK_NULL_HANDLE)
		{
			VkAttachmentReference depth_reference;
			VkAttachmentDescription attachments[1] = {{0}};
			VkSubpassDescription subpass = {0};
			VkRenderPassCreateInfo rp_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

			depth_reference.attachment = 0;
			depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			attachments[depth_reference.attachment].format = imginfo.format;
			attachments[depth_reference.attachment].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[depth_reference.attachment].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachments[depth_reference.attachment].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[depth_reference.attachment].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[depth_reference.attachment].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[depth_reference.attachment].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			attachments[depth_reference.attachment].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.flags = 0;
			subpass.inputAttachmentCount = 0;
			subpass.pInputAttachments = NULL;
			subpass.colorAttachmentCount = 0;
			subpass.pColorAttachments = NULL;
			subpass.pResolveAttachments = NULL;
			subpass.pDepthStencilAttachment = &depth_reference;
			subpass.preserveAttachmentCount = 0;
			subpass.pPreserveAttachments = NULL;

			rp_info.attachmentCount = countof(attachments);
			rp_info.pAttachments = attachments;
			rp_info.subpassCount = 1;
			rp_info.pSubpasses = &subpass;
			rp_info.dependencyCount = 0;
			rp_info.pDependencies = NULL;

			VkAssert(vkCreateRenderPass(vk.device, &rp_info, vkallocationcb, &vk.shadow_renderpass));
		}

		fbinfo.flags = 0;
		fbinfo.renderPass = vk.shadow_renderpass;
		fbinfo.attachmentCount = 1;
		fbinfo.width = width;
		fbinfo.height = height;
		fbinfo.layers = 1;
		for (i = 0; i < countof(shad->buf); i++)
		{
			VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			ivci.format = imginfo.format;
			ivci.components.r = VK_COMPONENT_SWIZZLE_R;
			ivci.components.g = VK_COMPONENT_SWIZZLE_G;
			ivci.components.b = VK_COMPONENT_SWIZZLE_B;
			ivci.components.a = VK_COMPONENT_SWIZZLE_A;
			ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			ivci.subresourceRange.baseMipLevel = 0;
			ivci.subresourceRange.levelCount = 1;
			ivci.subresourceRange.baseArrayLayer = i;
			ivci.subresourceRange.layerCount = 1;
			ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			ivci.flags = 0;
			ivci.image = shad->image;
			shad->buf[i].vimage.image = shad->image;
			VkAssert(vkCreateImageView(vk.device, &ivci, vkallocationcb, &shad->buf[i].vimage.view));

			{
				VkSamplerCreateInfo lmsampinfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
				lmsampinfo.minFilter = lmsampinfo.magFilter = VK_FILTER_LINEAR;
				lmsampinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				lmsampinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				lmsampinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				lmsampinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				lmsampinfo.mipLodBias = 0.0;
				lmsampinfo.anisotropyEnable = VK_FALSE;
				lmsampinfo.maxAnisotropy = 0;
				lmsampinfo.compareEnable = VK_TRUE;
				lmsampinfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
				lmsampinfo.minLod = 0;
				lmsampinfo.maxLod = 0;
				lmsampinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				lmsampinfo.unnormalizedCoordinates = VK_FALSE;
				VkAssert(vkCreateSampler(vk.device, &lmsampinfo, NULL, &shad->buf[i].vimage.sampler));
			}

			shad->buf[i].qimage.vkimage = &shad->buf[i].vimage;
			shad->buf[i].vimage.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

			fbinfo.pAttachments = &shad->buf[i].vimage.view;
			VkAssert(vkCreateFramebuffer(vk.device, &fbinfo, vkallocationcb, &shad->buf[i].framebuffer));
		}

		shad->width = width;
		shad->height = height;
	}

	sbuf = shad->seq++%countof(shad->buf);
	shaderstate.currentshadowmap = &shad->buf[sbuf].qimage;

	{
		VkImageMemoryBarrier imgbarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		imgbarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		imgbarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		imgbarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;	//we don't actually care because we'll be clearing it anyway, making this more of a no-op than anything else.
		imgbarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		imgbarrier.image = shad->buf[sbuf].vimage.image;
		imgbarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		imgbarrier.subresourceRange.baseMipLevel = 0;
		imgbarrier.subresourceRange.levelCount = 1;
		imgbarrier.subresourceRange.baseArrayLayer = sbuf;
		imgbarrier.subresourceRange.layerCount = 1;
		imgbarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imgbarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		vkCmdPipelineBarrier(vk.frame->cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &imgbarrier);
	}

	{
		VkClearValue clearval;
		VkRenderPassBeginInfo rpass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		clearval.depthStencil.depth = 1;
		clearval.depthStencil.stencil = 0;
		rpass.renderPass = vk.shadow_renderpass;
		rpass.framebuffer = shad->buf[sbuf].framebuffer;
		rpass.renderArea.offset.x = 0;
		rpass.renderArea.offset.y = 0;
		rpass.renderArea.extent.width = width;
		rpass.renderArea.extent.height = height;
		rpass.clearValueCount = 1;
		rpass.pClearValues = &clearval;
		vkCmdBeginRenderPass(vk.frame->cbuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);
	}

	//viewport+scissor will be done elsewhere
	//that wasn't too painful, was it?...
	return true;
}

void VKBE_DoneShadows(void)
{
//	struct shadowmaps_s *shad = &shaderstate.shadow[isspot];
	VkViewport viewport;

//	const qboolean altqueue = false;

	//we've rendered the shadowmap, but now we need to blit it to the screen
	//so set stuff back to the main view. FIXME: do these in batches to ease the load on tilers.
	vkCmdEndRenderPass(vk.frame->cbuf);

	/*if (altqueue)
	{
		vkCmdSetEvent(alt, shadowcompleteevent);
		VKBE_FlushDynamicBuffers();
		VK_Submit_Work();
		vkCmdWaitEvents(main, 1, &shadowcompleteevent, barrierstuff);
		vkCmdResetEvent(main, shadowcompleteevent);
	}
	else*/
	{
		/*
		set_image_layout(vk.frame->cbuf, shad->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);

		{
			VkImageMemoryBarrier imgbarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			imgbarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			imgbarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			imgbarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			imgbarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			imgbarrier.image = image;
			imgbarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			imgbarrier.subresourceRange.baseMipLevel = 0;
			imgbarrier.subresourceRange.levelCount = 1;
			imgbarrier.subresourceRange.baseArrayLayer = 0;
			imgbarrier.subresourceRange.layerCount = 1;
			imgbarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgbarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &imgbarrier);
		}
		*/

		vkCmdBeginRenderPass(vk.frame->cbuf, &vk.rendertarg->restartinfo, VK_SUBPASS_CONTENTS_INLINE);

		viewport.x = r_refdef.pxrect.x;
		viewport.y = r_refdef.pxrect.y;//r_refdef.pxrect.maxheight - (r_refdef.pxrect.y+r_refdef.pxrect.height);	//silly GL...
		viewport.width = r_refdef.pxrect.width;
		viewport.height = r_refdef.pxrect.height;
		viewport.minDepth = 0;
		viewport.maxDepth = shaderstate.depthrange;
		vkCmdSetViewport(vk.frame->cbuf, 0, 1, &viewport);
	}


	VKBE_SelectEntity(&r_worldentity);
}

void VKBE_SetupForShadowMap(dlight_t *dl, qboolean isspot, int texwidth, int texheight, float shadowscale)
{
#define SHADOWMAP_SIZE 512
	extern cvar_t r_shadow_shadowmapping_nearclip, r_shadow_shadowmapping_bias;
	float nc = r_shadow_shadowmapping_nearclip.value;
	float bias = r_shadow_shadowmapping_bias.value;

	//much of the projection matrix cancels out due to symmetry and stuff
	//we need to scale between -0.5,0.5 within the sub-image. the fragment shader will center on the subimage based upon the major axis.
	//in d3d, the depth value is scaled between 0 and 1 (gl is -1 to 1).
	//d3d's framebuffer is upside down or something annoying like that.
	shaderstate.lightshadowmapproj[0] = shadowscale * (1.0-(1.0/texwidth)) * 0.5/3.0;	//pinch x inwards
	shaderstate.lightshadowmapproj[1] = -shadowscale * (1.0-(1.0/texheight)) * 0.5/2.0;	//pinch y inwards
	shaderstate.lightshadowmapproj[2] = 0.5*(dl->radius+nc)/(nc-dl->radius);	//proj matrix 10
	shaderstate.lightshadowmapproj[3] = (dl->radius*nc)/(nc-dl->radius) - bias*nc*(1024/texheight);	//proj matrix 14	

	shaderstate.lightshadowmapscale[0] = 1.0/(SHADOWMAP_SIZE*3);
	shaderstate.lightshadowmapscale[1] = -1.0/(SHADOWMAP_SIZE*2);
}

//FIXME: needs context for threading
void VKBE_BeginShadowmapFace(void)
{
	VkRect2D wrekt;
	VkViewport viewport;

	viewport.x = r_refdef.pxrect.x;
	viewport.y = r_refdef.pxrect.maxheight - (r_refdef.pxrect.y+r_refdef.pxrect.height);	//silly GL...
	viewport.width = r_refdef.pxrect.width;
	viewport.height = r_refdef.pxrect.height;
	viewport.minDepth = 0;
	viewport.maxDepth = 1;
	vkCmdSetViewport(vk.frame->cbuf, 0, 1, &viewport);

	wrekt.offset.x = viewport.x;
	wrekt.offset.y = viewport.y;
	wrekt.extent.width = viewport.width;
	wrekt.extent.height = viewport.height;
	vkCmdSetScissor(vk.frame->cbuf, 0, 1, &wrekt);
}
#endif

void VKBE_DrawWorld (batch_t **worldbatches)
{
	batch_t *batches[SHADER_SORT_COUNT];
	RSpeedLocals();

	shaderstate.curentity = NULL;

	shaderstate.depthrange = 0;

	if (!r_refdef.recurse)
	{
		if (shaderstate.wbatch > shaderstate.maxwbatches)
		{
			int newm = shaderstate.wbatch;
			Z_Free(shaderstate.wbatches);
			shaderstate.wbatches = Z_Malloc(newm * sizeof(*shaderstate.wbatches));
			memset(shaderstate.wbatches + shaderstate.maxwbatches, 0, (newm - shaderstate.maxwbatches) * sizeof(*shaderstate.wbatches));
			shaderstate.maxwbatches = newm;
		}
		shaderstate.wbatch = 0;
	}

	RSpeedRemark();

	shaderstate.curdlight = NULL;
	//fixme: figure out some way to safely orphan this data so that we can throw the rest to a worker.
	BE_GenModelBatches(batches, shaderstate.curdlight, BEM_STANDARD);

	BE_UploadLightmaps(false);
	if (r_refdef.scenevis)
	{
		//make sure the world draws correctly
		r_worldentity.shaderRGBAf[0] = 1;
		r_worldentity.shaderRGBAf[1] = 1;
		r_worldentity.shaderRGBAf[2] = 1;
		r_worldentity.shaderRGBAf[3] = 1;
		r_worldentity.axis[0][0] = 1;
		r_worldentity.axis[1][1] = 1;
		r_worldentity.axis[2][2] = 1;

#ifdef RTLIGHTS
		if (r_refdef.scenevis && r_shadow_realtime_world.ival)
			shaderstate.identitylighting = r_shadow_realtime_world_lightmaps.value;
		else
#endif
			shaderstate.identitylighting = 1;
		shaderstate.identitylighting *= r_refdef.hdr_value;
		shaderstate.identitylightmap = shaderstate.identitylighting / (1<<gl_overbright.ival);

		if (r_lightprepass)
		{
			//set up render target for gbuffer
			//draw opaque gbuffers
			//switch render targets to lighting (renderpasses?)
			//draw lpp lights
			//revert to screen
			//draw opaques again.
		}
		else
		{
			VKBE_SelectMode(BEM_STANDARD);

			
			VKBE_SubmitMeshes(worldbatches, batches, SHADER_SORT_PORTAL, SHADER_SORT_SEETHROUGH+1);
			RSpeedEnd(RSPEED_WORLD);

#ifdef RTLIGHTS
			RSpeedRemark();
			VKBE_SelectEntity(&r_worldentity);
			Sh_DrawLights(r_refdef.scenevis);
			RSpeedEnd(RSPEED_STENCILSHADOWS);
#endif
		}

		VKBE_SubmitMeshes(worldbatches, batches, SHADER_SORT_SEETHROUGH+1, SHADER_SORT_COUNT);


		if (r_wireframe.ival)
		{
			VKBE_SelectMode(BEM_WIREFRAME);
			VKBE_SubmitMeshes(worldbatches, batches, SHADER_SORT_PORTAL, SHADER_SORT_NEAREST);
			VKBE_SelectMode(BEM_STANDARD);
		}
	}
	else
	{
		shaderstate.identitylighting = 1;
		shaderstate.identitylightmap = 1;
		VKBE_SubmitMeshes(NULL, batches, SHADER_SORT_PORTAL, SHADER_SORT_COUNT);
		RSpeedEnd(RSPEED_DRAWENTITIES);
	}

	R_RenderDlights ();

	shaderstate.identitylighting = 1;

	BE_RotateForEntity(&r_worldentity, NULL);
}

void VKBE_VBO_Begin(vbobctx_t *ctx, size_t maxsize)
{
	struct stagingbuf *n = Z_Malloc(sizeof(*n));
	ctx->vboptr[0] = n;
	ctx->maxsize = maxsize;
	ctx->pos = 0;

	ctx->fallback = VKBE_CreateStagingBuffer(n, maxsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

	//preallocate the target buffer, so we can prematurely refer to it.
	{
		VkBufferCreateInfo bufinf = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

		bufinf.flags = 0;
		bufinf.size = n->size;
		bufinf.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufinf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bufinf.queueFamilyIndexCount = 0;
		bufinf.pQueueFamilyIndices = NULL;
		vkCreateBuffer(vk.device, &bufinf, vkallocationcb, &n->retbuf);
	}
}
void VKBE_VBO_Data(vbobctx_t *ctx, void *data, size_t size, vboarray_t *varray)
{
	struct stagingbuf *n = ctx->vboptr[0];
	varray->vk.offs = ctx->pos;
	varray->vk.buff = n->retbuf;
	ctx->pos += size;

	memcpy((char*)ctx->fallback + varray->vk.offs, data, size);
}
void VKBE_VBO_Finish(vbobctx_t *ctx, void *edata, size_t esize, vboarray_t *earray, void **vbomem, void **ebomem)
{
	struct stagingbuf *n;
	struct stagingbuf ebo;
	VkDeviceMemory *retarded;
	index_t *map = VKBE_CreateStagingBuffer(&ebo, esize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	memcpy(map, edata, esize);
	*ebomem = retarded = Z_Malloc(sizeof(*retarded));
	earray->vk.buff = VKBE_FinishStaging(&ebo, retarded);
	earray->vk.offs = 0;

	if (ctx)
	{
		n = ctx->vboptr[0];
		*vbomem = retarded = Z_Malloc(sizeof(*retarded));
		VKBE_FinishStaging(n, retarded);
		Z_Free(n);
	}
}
void VKBE_VBO_Destroy(vboarray_t *vearray, void *mem)
{
	VkDeviceMemory *retarded = mem;
	struct fencedbufferwork *fence;
	if (!vearray->vk.buff)
		return;	//not actually allocated...

	fence = VK_AtFrameEnd(VKBE_DoneBufferStaging, sizeof(*fence));
	fence->buf = vearray->vk.buff;
	fence->mem = *retarded;

	Z_Free(retarded);
}

void VKBE_Scissor(srect_t *rect)
{
	VkRect2D wrekt;
	if (rect)
	{
		wrekt.offset.x = rect->x * vid.fbpwidth;
		wrekt.offset.y = (1 - (rect->height + rect->y))*vid.fbpheight;  //our api was made for gl. :(
		wrekt.extent.width = rect->width * vid.fbpwidth;
		wrekt.extent.height = rect->height * vid.fbpheight;

		if (wrekt.offset.x+wrekt.extent.width > vid.fbpwidth)
			wrekt.extent.width = vid.fbpwidth - wrekt.offset.x;
		if (wrekt.offset.y+wrekt.extent.height > vid.fbpheight)
			wrekt.extent.height = vid.fbpheight - wrekt.offset.y;
		if (wrekt.offset.x < 0)
		{
			wrekt.extent.width += wrekt.offset.x;
			wrekt.offset.x = 0;
		}
		if (wrekt.offset.y < 0)
		{
			wrekt.extent.height += wrekt.offset.x;
			wrekt.offset.y = 0;
		}
	}
	else
	{
		wrekt.offset.x = 0;
		wrekt.offset.y = 0;
		wrekt.extent.width = vid.fbpwidth;
		wrekt.extent.height = vid.fbpheight;
	}

	vkCmdSetScissor(vk.frame->cbuf, 0, 1, &wrekt);
}

#endif
