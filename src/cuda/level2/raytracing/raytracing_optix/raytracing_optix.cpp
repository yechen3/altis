// ======================================================================== //
// Copyright 2018 Ingo Wald                                                 //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

// ooawe
#include "programs/vec.h"
#include "savePPM.h"
// optix
#include <optix.h>
#include <optixu/optixpp.h>
// std
#define _USE_MATH_DEFINES 1
#include <math.h>
#include <cmath>
#include <random>
#include <iostream>
#include <iomanip>
#include <chrono>

#include "OptionParser.h"
#include "ResultDatabase.h"
// #include "cudacommon.h" Cause compilation err

optix::Context g_context;

/*! the precompiled programs/raygen.cu code (in ptx) that our
  cmake magic will precompile (to ptx) and link to the generated
  executable (ie, we can simply declare and usethis here as
  'extern'.  */
extern "C" const char embedded_sphere_programs[];
extern "C" const char embedded_raygen_programs[];
extern "C" const char embedded_miss_programs[];
extern "C" const char embedded_metal_programs[];
extern "C" const char embedded_dielectric_programs[];
extern "C" const char embedded_lambertian_programs[];

float rnd()
{
  // static std::random_device rd;  //Will be used to obtain a seed for the random number engine
  static std::mt19937 gen(0); //Standard mersenne_twister_engine seeded with rd()
  static std::uniform_real_distribution<float> dis(0.f, 1.f);
  return dis(gen);
}

/*! abstraction for a material that can create, and parameterize,
  a newly created GI's material and closest hit program */
struct Material {
  virtual void assignTo(optix::GeometryInstance gi) const = 0;
};

/*! host side code for the "Lambertian" material; the actual
  sampling code is in the programs/lambertian.cu closest hit program */
struct Lambertian : public Material {
  /*! constructor */
  Lambertian(const vec3f &albedo) : albedo(albedo) {}
  /* create optix material, and assign mat and mat values to geom instance */
  virtual void assignTo(optix::GeometryInstance gi) const override {
    optix::Material mat = g_context->createMaterial();
    mat->setClosestHitProgram(0, g_context->createProgramFromPTXString
                              (embedded_lambertian_programs,
                               "closest_hit"));
    gi->setMaterial(/*ray type:*/0, mat);
    gi["albedo"]->set3fv(&albedo.x);
  }
  const vec3f albedo;
};

/*! host side code for the "Metal" material; the actual
  sampling code is in the programs/metal.cu closest hit program */
struct Metal : public Material {
  /*! constructor */
  Metal(const vec3f &albedo, const float fuzz) : albedo(albedo), fuzz(fuzz) {}
  /* create optix material, and assign mat and mat values to geom instance */
  virtual void assignTo(optix::GeometryInstance gi) const override {
    optix::Material mat = g_context->createMaterial();
    mat->setClosestHitProgram(0, g_context->createProgramFromPTXString
                              (embedded_metal_programs,
                               "closest_hit"));
    gi->setMaterial(/*ray type:*/0, mat);
    gi["albedo"]->set3fv(&albedo.x);
    gi["fuzz"]->setFloat(fuzz);
  }
  const vec3f albedo;
  const float fuzz;
};

/*! host side code for the "Dielectric" material; the actual
  sampling code is in the programs/dielectric.cu closest hit program */
struct Dielectric : public Material {
  /*! constructor */
  Dielectric(const float ref_idx) : ref_idx(ref_idx) {}
  /* create optix material, and assign mat and mat values to geom instance */
  virtual void assignTo(optix::GeometryInstance gi) const override {
    optix::Material mat = g_context->createMaterial();
    mat->setClosestHitProgram(0, g_context->createProgramFromPTXString
                              (embedded_dielectric_programs,
                               "closest_hit"));
    gi->setMaterial(/*ray type:*/0, mat);
    gi["ref_idx"]->setFloat(ref_idx);
  }
  const float ref_idx;
};

optix::GeometryInstance createSphere(const vec3f &center, const float radius, const Material &material)
{
  optix::Geometry geometry = g_context->createGeometry();
  geometry->setPrimitiveCount(1);
  geometry->setBoundingBoxProgram
    (g_context->createProgramFromPTXString(embedded_sphere_programs, "get_bounds"));
  geometry->setIntersectionProgram
    (g_context->createProgramFromPTXString(embedded_sphere_programs, "hit_sphere"));
  geometry["center"]->setFloat(center.x,center.y,center.z);
  geometry["radius"]->setFloat(radius);

  optix::GeometryInstance gi = g_context->createGeometryInstance();
  gi->setGeometry(geometry);
  gi->setMaterialCount(1);
  material.assignTo(gi);
  return gi;
}

optix::GeometryGroup createScene()
{ 
  // first, create all geometry instances (GIs), and, for now,
  // store them in a std::vector. For ease of reference, I'll
  // stick wit the 'd_list' and 'd_world' names used in the
  // reference C++ and CUDA codes.
  std::vector<optix::GeometryInstance> d_list;

  d_list.push_back(createSphere(vec3f(0.f, -1000.0f, -1.f), 1000.f,
                                Lambertian(vec3f(0.5f, 0.5f, 0.5f))));

  for (int a = -11; a < 11; a++) {
    for (int b = -11; b < 11; b++) {
      float choose_mat = rnd();
      vec3f center(a + rnd(), 0.2f, b + rnd());
      if (choose_mat < 0.8f) {
        d_list.push_back(createSphere(center, 0.2f,
                                      Lambertian(vec3f(rnd()*rnd(), rnd()*rnd(), rnd()*rnd()))));
      }
      else if (choose_mat < 0.95f) {
        d_list.push_back(createSphere(center, 0.2f,
                                      Metal(vec3f(0.5f*(1.0f + rnd()), 0.5f*(1.0f + rnd()), 0.5f*(1.0f + rnd())), 0.5f*rnd())));
      }
      else {
        d_list.push_back(createSphere(center, 0.2f, Dielectric(1.5f)));
      }
    }
  }
  d_list.push_back(createSphere(vec3f(0.f, 1.f, 0.f), 1.f, Dielectric(1.5f)));
  d_list.push_back(createSphere(vec3f(-4.f, 1.f, 0.f), 1.f, Lambertian(vec3f(0.4f, 0.2f, 0.1f))));
  d_list.push_back(createSphere(vec3f(4.f, 1.f, 0.f), 1.f, Metal(vec3f(0.7f, 0.6f, 0.5f), 0.0f)));
  
  // now, create the optix world that contains all these GIs
  optix::GeometryGroup d_world = g_context->createGeometryGroup();
  d_world->setAcceleration(g_context->createAcceleration("Bvh"));
  d_world->setChildCount((int)d_list.size());
  for (int i = 0; i < d_list.size(); i++)
    d_world->setChild(i, d_list[i]);

  // that all we have to do, the rest is up to optix
  return d_world;
}

struct Camera {
  Camera(const vec3f &lookfrom, const vec3f &lookat, const vec3f &vup, 
         float vfov, float aspect, float aperture, float focus_dist) 
  { // vfov is top to bottom in degrees
    lens_radius = aperture / 2.0f;
    float theta = vfov * ((float)M_PI) / 180.0f;
    float half_height = tan(theta / 2.0f);
    float half_width = aspect * half_height;
    origin = lookfrom;
    w = unit_vector(lookfrom - lookat);
    u = unit_vector(cross(vup, w));
    v = cross(w, u);
    lower_left_corner = origin - half_width * focus_dist*u - half_height * focus_dist*v - focus_dist * w;
    horizontal = 2.0f*half_width*focus_dist*u;
    vertical = 2.0f*half_height*focus_dist*v;
  }
	
  void set()
  {
    g_context["camera_lower_left_corner"]->set3fv(&lower_left_corner.x);
    g_context["camera_horizontal"]->set3fv(&horizontal.x);
    g_context["camera_vertical"]->set3fv(&vertical.x);
    g_context["camera_origin"]->set3fv(&origin.x);
    g_context["camera_u"]->set3fv(&u.x);
    g_context["camera_v"]->set3fv(&v.x);
    g_context["camera_w"]->set3fv(&w.x);
    g_context["camera_lens_radius"]->setFloat(lens_radius);
  }
  vec3f origin;
  vec3f lower_left_corner;
  vec3f horizontal;
  vec3f vertical;
  vec3f u, v, w;
  float lens_radius;
};


void renderFrame(int Nx, int Ny)
{
  // ... and validate everything before launch.
  g_context->validate();

  // now that everything is set up: launch that ray generation program
  g_context->launch(/*program ID:*/0,
                    /*launch dimensions:*/Nx, Ny);
}

optix::Buffer createFrameBuffer(int Nx, int Ny)
{
  // ... create an image - as a 2D buffer of float3's ...
  optix::Buffer pixelBuffer
    = g_context->createBuffer(RT_BUFFER_OUTPUT);
  pixelBuffer->setFormat(RT_FORMAT_FLOAT3);
  pixelBuffer->setSize(Nx, Ny);
  return pixelBuffer;
}

void setRayGenProgram()
{
  optix::Program rayGenAndBackgroundProgram
    = g_context->createProgramFromPTXString(embedded_raygen_programs,
                                            "renderPixel");
  g_context->setEntryPointCount(1);
  g_context->setRayGenerationProgram(/*program ID:*/0, rayGenAndBackgroundProgram);
}

void setMissProgram()
{
  optix::Program missProgram
    = g_context->createProgramFromPTXString(embedded_miss_programs,
                                            "miss_program");
  g_context->setMissProgram(/*program ID:*/0, missProgram);
}

void addBenchmarkSpecOptions(OptionParser &op) {
    op.addOption("X", OPT_INT, "1200", "specify image x dimension", '\0');
    op.addOption("Y", OPT_INT, "800", "specify image y dimension", '\0');
    op.addOption("samples", OPT_INT, "10", "specify number of iamge samples", '\0');
}

void RunBenchmark(ResultDatabase &DB, OptionParser &op)
{
    /* cudaEvent_t total_start, total_stop; */
    /* cudaEvent_t start, stop; */
    /* checkCudaErrors(cudaEventCreate(&start)); */
    /* checkCudaErrors(cudaEventCreate(&stop)); */
    
    /* checkCudaErrors(cudaEventCreate(&total_start)); */
    /* checkCudaErrors(cudaEventCreate(&total_stop)); */ 
    /* checkCudaErrors(cudaEventRecord(total_start, 0)); */


    // before doing anything else: create a optix context
    g_context = optix::Context::create();
    g_context->setRayTypeCount(1);
    g_context->setStackSize( 8000 );

    // define some image size ...
    size_t xDim[4] = {400, 1200, 4096, 15360};
    size_t yDim[4] = {300, 800, 2160, 8640};
    size_t size = (size_t)op.getOptionInt("size") - 1;
    size_t Nx = xDim[size];
    size_t Ny = yDim[size];
    if (op.getOptionInt("X") != 1200 || op.getOptionInt("Y") != 800) {
        Nx = (size_t)op.getOptionInt("X");
        Ny = (size_t)op.getOptionInt("Y");
    }
    int numSamples = op.getOptionInt("samples");
    assert(numSamples > 0);
    int num_passes = op.getOptionInt("passes");

    // create - and set - the camera
    const vec3f lookfrom(13, 2, 3);
    const vec3f lookat(0, 0, 0);
    Camera camera(lookfrom,
                lookat,
                /* up */ vec3f(0, 1, 0),
                /* fovy, in degrees */ 20.0,
                /* aspect */ float(Nx) / float(Ny),
                /* aperture */ 0.1f,
                /* dist to focus: */ 10.f);
    camera.set();

    // set the ray generation and miss shader program
    setRayGenProgram();
    setMissProgram();

    // create a frame buffer
    optix::Buffer fb = createFrameBuffer(Nx, Ny);
    g_context["fb"]->set(fb);

    // create the world to render
    optix::GeometryGroup world = createScene();
    g_context["world"]->set(world);

    // const int numSamples = 128;
    g_context["numSamples"]->setInt(numSamples);

    char atts[1024];
    sprintf(atts, "img: %zu by %zu, samples: %d, iter:%d", Nx, Ny, num_passes, num_passes);
#if 1
  {
    // Note: this little piece of code (in the #if 1/#endif bracket)
    // is _NOT_ required for correctness; it's just been added to
    // factor our build time vs render time: In optix, the data
    // structure gets built 'on demand', which basically means it gets
    // built on the first launch after the scene got set up or
    // changed. Thus, if we create a 0-sized launch optix won't do any
    // rendering (0 pixels to render), but will still build; while the
    // second renderFrame call below won't do any build (already done)
    // but do all the rendering (Nx*Ny pixels) ...
    auto t0 = std::chrono::system_clock::now();
    renderFrame(0,0);
    auto t1 = std::chrono::system_clock::now();
    DB.AddResult("Optix Data Structures Building Time", atts, "sec", std::chrono::duration<float>(t1-t0).count());
  }
#endif

  int i = 0;
  for (; i < num_passes; i++) {
      // render the frame (and time it)
      auto t0 = std::chrono::system_clock::now();
      renderFrame(Nx, Ny);
      auto t1 = std::chrono::system_clock::now();
#if 0
      // ... map it, save it, and cleanly unmap it after reading...
      const vec3f *pixels = (const vec3f *)fb->map();
      savePPM("finalChapter.ppm",Nx,Ny,pixels);
      fb->unmap();
#endif
      DB.AddResult("Rendering Time", atts, "sec", std::chrono::duration<float>(t1-t0).count());
  }
  // ... done.
}

