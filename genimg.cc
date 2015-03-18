#undef _FORTIFY_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <librsvg/rsvg.h>
#include <cairo.h>

#include <vector>
#include <list>
#include <set>
#include <string>
#include <map>

using namespace std;

void reduce(unsigned char *dst, const unsigned char *src, int sx, int sy, int ssx, int ssy, int ratio)
{
  for(int yy=0; yy<ssy; yy++)
    for(int xx=0; xx<ssx; xx++) {
      int r=0, g=0, b=0;
      for(int y=0; y<ratio; y++) {
	const unsigned char *src1 = src + 4*(sx*(ratio*yy+y) + ratio*xx);
	for(int x=0; x<ratio; x++) {
	  r += src1[2];
	  g += src1[1];
	  b += src1[0];
	  src1 += 4;
	}
      }
      *dst++ = r/(ratio*ratio);
      *dst++ = g/(ratio*ratio);
      *dst++ = b/(ratio*ratio);
    }
}



static void w32(unsigned char *p, int l)
{
  p[0] = l>>24;
  p[1] = l>>16;
  p[2] = l>>8;
  p[3] = l;
}

static void wchunk(int fd, unsigned int type, unsigned char *p, unsigned int l)
{
  unsigned char v[8];
  unsigned int crc;
  w32(v, l);
  w32(v+4, type);
  write(fd, v, 8);
  crc = crc32(0, v+4, 4);
  if(l) {
    write(fd, p, l);
    crc = crc32(crc, p, l);
  }
  w32(v, crc);
  write(fd, v, 4);
}

void png_write(const char *name, const void *data, int width, int height, bool has_alpha)
{
  char msg[4096];
  sprintf(msg, "Error opening %s for writing", name);
  int fd = open(name, O_WRONLY|O_CREAT|O_TRUNC, 0666);
  if(fd<0) {
    perror(msg);
    exit(1);
  }

  int ps = has_alpha ? 4 : 3;
  const unsigned char *edata = (const unsigned char *)data;
  unsigned char *image = new unsigned char[(width*ps+1)*height];
  unsigned char *ddata = image;

  for(int y=0; y<height; y++) {
    *ddata++ = 0;
    memcpy(ddata, edata, width*ps);
    ddata += width*ps;
    edata += width*ps;
  }  

  unsigned long sz = (int)((width*ps+1)*height*1.1+12);
  unsigned char *cdata = new unsigned char[sz];

  write(fd, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8);

  w32(cdata, width);
  w32(cdata+4, height);
  cdata[8] = 8;
  cdata[9] = has_alpha ? 6 : 2;
  cdata[10] = 0;
  cdata[11] = 0;
  cdata[12] = 0;
  wchunk(fd, 0x49484452L, cdata, 13); // IHDR

  compress(cdata, &sz, image, (width*ps+1)*height);
  wchunk(fd, 0x49444154L, cdata, sz); // IDAT
  wchunk(fd, 0x49454E44L, 0, 0); // IEND
  close(fd);

  delete[] cdata;
  delete[] image;
}

struct object_entry {
  string name;
  int spos, epos;
    int x0, y0, x1, y1, nsx, nsy;
  object_entry(string n, int s, int e) { name = n; spos = s; epos = e; }
};

void find_tags_titles(const vector<char> &svg, list<object_entry> &objects)
{
  vector<int> spos_stack;
  vector<int> epos_stack;
  int obj_stack_pos = -1;
  string title;
  int pos = 0;
  int epos = svg.size();
  while(pos < epos) {
    while(pos < epos && svg[pos] != '<')
      pos++;
    if(pos >= epos)
      break;
    bool opening = false, closing = false;
    int tag_spos = pos;
    pos++;
    if(pos >= epos)
      break;
    if(svg[pos] == '/') {
      closing = true;
      pos++;
      if(pos >= epos)
	break;
    } else
      opening = true;
    while(pos < epos && (svg[pos] == ' ' || svg[pos] == '\t' || svg[pos] == '\r' || svg[pos] == '\n'))
      pos++;
    int tstart = pos;
    while(pos < epos && svg[pos] != ' ' && svg[pos] != '\t' && svg[pos] != '\r' && svg[pos] != '\n' && svg[pos] != '/' && svg[pos] != '>')
      pos++;
    if(pos >= epos)
      break;
    string tag(&svg[tstart], pos-tstart);
    int scanpos = pos;
    while(pos < epos && svg[pos] != '>')
      pos++;
    if(pos >= epos)
      break;
    int tag_epos = pos+1;
    int xpos = pos++;
    while(--xpos >= scanpos) {
      if(svg[xpos] == '/') {
	closing = true;
	break;
      }
      if(svg[xpos] != ' ' && svg[xpos] != '\t' && svg[xpos] != '\r' && svg[xpos] != '\n')
	break;
    }
    if(tag == "?xml" || tag == "!--")
      continue;

    if(opening && !closing) {
      spos_stack.push_back(tag_spos);
      epos_stack.push_back(tag_epos);

    } else if(closing && !opening) {
      if(tag == "title") {
	title = string(&svg[epos_stack.back()], tag_spos - epos_stack.back());
	obj_stack_pos = spos_stack.size()-1;
      }
      if(int(spos_stack.size()) == obj_stack_pos) {
	objects.push_back(object_entry(title, spos_stack.back(), tag_epos));
	obj_stack_pos = -1;
      }
      spos_stack.pop_back();
      epos_stack.pop_back();
    }
  }
}

void generate_image(unsigned char *destimg, const vector<char> &svg, const list<object_entry> &objects, cairo_t *cr, cairo_surface_t *surface, int sx, int sy, int ssx, int ssy, int ratio, string tag)
{
  vector<char> gsvg;
  gsvg.resize(svg.size());
  int dpos = 0;
  int ppos = 0;
  for(list<object_entry>::const_iterator i = objects.begin(); i != objects.end(); i++) {
    if(i->name == tag)
      continue;
    int sz = i->spos - ppos;
    memcpy(&gsvg[dpos], &svg[ppos], sz);
    dpos += sz;
    ppos = i->epos;
  }
  int sz = int(svg.size()) - ppos;
  memcpy(&gsvg[dpos], &svg[ppos], sz);
  dpos += sz;
  gsvg.resize(dpos);

  GError *error = NULL;
  RsvgHandle *handle = rsvg_handle_new_with_flags(RSVG_HANDLE_FLAG_UNLIMITED);
    
  rsvg_handle_write(handle, (const guint8 *)&gsvg[0], gsvg.size(), &error);
  rsvg_handle_close(handle, &error);
  if(error != NULL) {
    fprintf(stderr, "rvsg parsing failure %s\n", error->message);
    exit(1);
  }

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0, 0, 0, 0);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_restore (cr);
  rsvg_handle_render_cairo(handle, cr);

  cairo_status_t status = cairo_status(cr);
  if(status) {
    fprintf(stderr, "Cairo failure: %s\n", cairo_status_to_string(status));
    exit(1);
  }

  cairo_surface_flush(surface);
  reduce(destimg, cairo_image_surface_get_data(surface), sx, sy, ssx, ssy, ratio);
  rsvg_handle_close(handle, &error);
}

void generate_diff(unsigned char *destimg, const unsigned char *cimg, const unsigned char *bgimg, int ssx, int ssy, int &x0, int &y0, int &x1, int &y1, int &nsx, int &nsy)
{
  x0 = ssx-1;
  x1 = 0;
  y0 = ssy-1;
  y1 = 0;
  const unsigned char *c1 = cimg;
  const unsigned char *b1 = bgimg;
  for(int y=0; y<ssy; y++)
    for(int x=0; x<ssx; x++) {
      if(c1[0] != b1[0] || c1[1] != b1[1] || c1[2] != b1[2]) {
	if(x < x0)
	  x0 = x;
	if(x > x1)
	  x1 = x;
	if(y < y0)
	  y0 = y;
	if(y > y1)
	  y1 = y;
      }
      c1 += 3;
      b1 += 3;
    }
  if(x1 < x0) {
    nsx = nsy = 0;
    return;
  }
  nsx = x1-x0+1;
  nsy = y1-y0+1;

  c1 =  cimg + 3*(x0 + y0*ssx);
  b1 = bgimg + 3*(x0 + y0*ssx);
  unsigned char *d1 = destimg;

  for(int y=y0; y<=y1; y++) {
    const unsigned char *c2 = c1;
    const unsigned char *b2 = b1;
    for(int x=x0; x<=x1; x++) {
      if(c2[0] != b2[0] || c2[1] != b2[1] || c2[2] != b2[2]) {
	*d1++ = c2[0];
	*d1++ = c2[1];
	*d1++ = c2[2];
	*d1++ = 255;
      } else {
	*d1++ = 0;
	*d1++ = 0;
	*d1++ = 0;
	*d1++ = 0;
      }
      c2 += 3;
      b2 += 3;
    }
    c1 += 3*ssx;
    b1 += 3*ssx;
  }
}

int main(int argc, char **argv)
{
  if(argc != 1) {
    fprintf(stderr, "Usage:\n%s\n", argv[0]);
    exit(1);
  }

  char buf[4096];
  vector<char> svg;
  list<object_entry> objects;
  vector<unsigned char> pbm;
  const char *fname = "dm53.svg";

  int sx = 6808;
  int sy = 9098;
  int ratio = 12;
  int ssx = sx/ratio;
  int ssy = sy/ratio;
  const char *destdir = "dm53";

  unsigned char *bgimg = (unsigned char *)malloc(3*ssx*ssy);
  unsigned char *cimg = (unsigned char *)malloc(3*ssx*ssy);
  unsigned char *dimg = (unsigned char *)malloc(4*ssx*ssy);

  sprintf(buf, "Open %s", fname);
  int fd = open(fname, O_RDONLY);
  if(fd<0) {
    perror(buf);
    exit(2);
  }

  int size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  svg.resize(size);
  read(fd, &svg[0], size);
  close(fd);

  find_tags_titles(svg, objects);

  rsvg_set_default_dpi(72.0);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sx, sy);
  cairo_t *cr = cairo_create(surface);

  generate_image(bgimg, svg, objects, cr, surface, sx, sy, ssx, ssy, ratio, "");

  char dname[4096];
  sprintf(dname, "%s/bg.png", destdir);
  png_write(dname, bgimg, ssx, ssy, false);

  set<string> seen;
  for(list<object_entry>::iterator i = objects.begin(); i != objects.end(); i++) {
    string n = i->name;
    if(seen.find(n) != seen.end())
      continue;
    seen.insert(n);
    generate_image(cimg, svg, objects, cr, surface, sx, sy, ssx, ssy, ratio, n);
    generate_diff(dimg, cimg, bgimg, ssx, ssy, i->x0, i->y0, i->x1, i->y1, i->nsx, i->nsy);
    if(!i->nsx) {
      fprintf(stderr, "Error: object %s is not visible\n", n.c_str());
      exit(1);
    }
    sprintf(dname, "%s/%s.png", destdir, n.c_str());
    png_write(dname, dimg, i->nsx, i->nsy, true);
    printf("%4d %4d: %s\n", i->x0, i->y0, n.c_str());
  }
  cairo_destroy(cr);

  sprintf(dname, "%s/default.lay", destdir);
  FILE *ofd = fopen(dname, "w");
  if(!ofd) {
    perror("opening default.lay");
    exit(1);
  }
  fprintf(ofd, "<?xml version=\"1.0\"?>\n<mamelayout version=\"2\">\n");
  fprintf(ofd, "<element name=\"bg\"><image file=\"bg.png\"/></element>\n");
  seen.clear();
  for(list<object_entry>::iterator i = objects.begin(); i != objects.end(); i++) {
    string n = i->name;
    if(seen.find(n) != seen.end())
      continue;
    seen.insert(n);
    fprintf(ofd, "<element name=\"%s\" defstate=\"0\"><image file=\"%s.png\"/></element>\n", n.c_str(), n.c_str());
  }
  
  fprintf(ofd, "<view name=\"lcd\">\n  <bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\n", ssx, ssy);
  fprintf(ofd, "  <bezel element=\"bg\">\n    <bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\n  </bezel>\n", ssx, ssy);
  seen.clear();
  for(list<object_entry>::iterator i = objects.begin(); i != objects.end(); i++) {
    string n = i->name;
    if(seen.find(n) != seen.end())
      continue;
    seen.insert(n);
    fprintf(ofd, "  <bezel name=\"b-%s\" element=\"%s\"><bounds x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"/></bezel>\n", n.c_str(), n.c_str(), i->x0, i->y0, i->nsx, i->nsy);
  }
  fprintf(ofd, "</view>\n</mamelayout>\n");
  fclose(ofd);
  return 0;
}
