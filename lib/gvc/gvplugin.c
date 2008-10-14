/* $Id$ $Revision$ */
/* vim:set shiftwidth=4 ts=8: */

/**********************************************************
*      This software is part of the graphviz package      *
*                http://www.graphviz.org/                 *
*                                                         *
*            Copyright (c) 1994-2004 AT&T Corp.           *
*                and is licensed under the                *
*            Common Public License, Version 1.0           *
*                      by AT&T Corp.                      *
*                                                         *
*        Information and Software Systems Research        *
*              AT&T Research, Florham Park NJ             *
**********************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include	<string.h>
#ifdef ENABLE_LTDL
#include	<ltdl.h>
#endif

#include        "memory.h"
#include        "types.h"
#include        "graph.h"
#include        "gvplugin.h"
#include        "gvcjob.h"
#include        "gvcint.h"
#include        "gvcproc.h"

#include	"const.h"

extern const int Demand_Loading;

/*
 * Define an apis array of name strings using an enumerated api_t as index.
 * The enumerated type is defined gvplugin.h.  The apis array is
 * inititialized here by redefining ELEM and reinvoking APIS.
 */
#define ELEM(x) #x,
static char *api_names[] = { APIS };	/* "render", "layout", ... */
#undef ELEM

/* translate a string api name to its type, or -1 on error */
api_t gvplugin_api(char *str)
{
    int api;

    for (api = 0; api < ARRAY_SIZE(api_names); api++) {
	if (strcmp(str, api_names[api]) == 0)
	    return (api_t)api;
    }
    return -1;			/* invalid api */
}

/* translate api_t into string name, or NULL */
char *gvplugin_api_name(api_t api)
{
    if (api < 0 || api >= ARRAY_SIZE(api_names))
	return NULL;
    return api_names[api];
}

/* install a plugin description into the list of available plugins
 * list is alpha sorted by type (not including :dependency), then
 * quality sorted within the type, then, if qualities are the same,
 * last install wins.
 */
boolean gvplugin_install(GVC_t * gvc, api_t api, const char *typestr,
	int quality, gvplugin_package_t *package,
	gvplugin_installed_t * typeptr)
{
    gvplugin_available_t *plugin, **pnext;
#define TYPSIZ 63
    char *p, pins[TYPSIZ+1], pnxt[TYPSIZ+1];

    if (api < 0)
	return FALSE;

    strncpy(pins, typestr, TYPSIZ);
    if ((p = strchr(pins, ':')))
	*p = '\0';
    
    /* point to the beginning of the linked list of plugins for this api */
    pnext = &(gvc->apis[api]);

    /* keep alpha-sorted and insert new duplicates ahead of old */
    while (*pnext) {
	strncpy(pnxt, (*pnext)->typestr, TYPSIZ);
	if ((p = strchr(pnxt, ':')))
	    *p = '\0';
	if (strcmp(pins, pnxt) <= 0)
	    break;
	pnext = &((*pnext)->next);
    }

    /* keep quality sorted within type and insert new duplicates ahead of old */
    while (*pnext) {
	strncpy(pnxt, (*pnext)->typestr, TYPSIZ);
	if ((p = strchr(pnxt, ':')))
	    *p = '\0';
	if (strcmp(pins, pnxt) != 0)
	    break;
	if (quality >= (*pnext)->quality)
	    break;
	pnext = &((*pnext)->next);
    }

    plugin = GNEW(gvplugin_available_t);
    plugin->next = *pnext;
    *pnext = plugin;
    plugin->typestr = typestr;
    plugin->quality = quality;
    plugin->package = package;
    plugin->typeptr = typeptr;	/* null if not loaded */

    return TRUE;
}

/* Activate a plugin description in the list of available plugins.
 * This is used when a plugin-library loaded because of demand for
 * one of its plugins. It updates the available plugin data with
 * pointers into the loaded library.
 * NB the quality value is not replaced as it might have been
 * manually changed in the config file.
 */
static boolean gvplugin_activate(GVC_t * gvc, api_t api,
		 const char *typestr, char *name, char *path,
		 gvplugin_installed_t * typeptr)
{
    gvplugin_available_t **pnext;


    if (api < 0)
	return FALSE;

    /* point to the beginning of the linked list of plugins for this api */
    pnext = &(gvc->apis[api]);

    while (*pnext) {
	if ( (strcasecmp(typestr, (*pnext)->typestr) == 0)
	  && (strcasecmp(name, (*pnext)->package->name) == 0)
	  && (strcasecmp(path, (*pnext)->package->path) == 0)) {
	    (*pnext)->typeptr = typeptr;
	    return TRUE;
	}
	pnext = &((*pnext)->next);
    }
    return FALSE;
}

gvplugin_library_t *gvplugin_library_load(GVC_t *gvc, char *path)
{
#ifdef ENABLE_LTDL
    lt_dlhandle hndl;
    lt_ptr ptr;
    char *s, *sym;
    int len;
    static char *p;
    static int lenp;
    char *libdir;
    char *suffix = "_LTX_library";

    if (!Demand_Loading)
	return NULL;

    libdir = gvconfig_libdir();
    len = strlen(libdir) + 1 + strlen(path) + 1;
    if (len > lenp) {
	lenp = len+20;
	if (p)
	    p = grealloc(p, lenp);
	else
	    p = gmalloc(lenp);
    }
	
#ifdef WIN32
    if (path[1] == ':') {
#else
    if (path[0] == '/') {
#endif
	strcpy(p, path);
    } else {
	strcpy(p, libdir);
	strcat(p, DIRSEP);
	strcat(p, path);
    }

    if (lt_dlinit()) {
        agerr(AGERR, "failed to init libltdl\n");
        return NULL;
    }
    hndl = lt_dlopen (p);
    if (!hndl) {
        agerr(AGWARN, "Could not load \"%s\" - %s\n", p, (char*)lt_dlerror());
        return NULL;
    }
    if (gvc->common.verbose >= 2)
	fprintf(stderr, "Loading %s\n", p);

	s = strrchr(p, DIRSEP[0]);
    len = strlen(s); 
#if defined(WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
    if (len < strlen("/gvplugin_x")) {
#else
    if (len < strlen("/libgvplugin_x")) {
#endif
	agerr (AGERR,"invalid plugin path \"%s\"\n", p);
	return NULL;
    }
    sym = gmalloc(len + strlen(suffix) + 1);
#if defined(WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
    strcpy(sym, s+1);         /* strip leading "/"  */
#else
    strcpy(sym, s+4);         /* strip leading "/lib" or "/cyg" */
#endif
#if defined(__CYGWIN__) || defined(__MINGW32__)
    s = strchr(sym, '-');     /* strip trailing "-1.dll" */
#else 
    s = strchr(sym, '.');     /* strip trailing ".so.0" or ".dll" or ".sl" */
#endif
    strcpy(s,suffix);         /* append "_LTX_library" */

    ptr = lt_dlsym (hndl, sym);
    if (!ptr) {
        agerr (AGERR,"failed to resolve %s in %s\n", sym, p);
	free(sym);
        return NULL;
    }
    free(sym);
    return (gvplugin_library_t *)(ptr);
#else
    agerr (AGERR,"dynamic loading not available\n");
    return NULL;
#endif
}


/* load a plugin of type=str
	the str can optionally contain one or more ":dependencies" 

	examples:
	        png
		png:cairo
        fully qualified:
		png:cairo:cairo
		png:cairo:gd
		png:gd:gd
      
*/
gvplugin_available_t *gvplugin_load(GVC_t * gvc, api_t api, const char *str)
{
    gvplugin_available_t **pnext, *rv;
    gvplugin_library_t *library;
    gvplugin_api_t *apis;
    gvplugin_installed_t *types;
#define TYPBUFSIZ 64
    char reqtyp[TYPBUFSIZ], typ[TYPBUFSIZ];
    char *reqdep, *dep = NULL, *reqpkg;
    int i;
    api_t apidep;

    /* check for valid apis[] index */
    if (api < 0)
	return NULL;

    if (api == API_device
	|| api == API_loadimage) /* api dependencies - FIXME - find better way to code these *s */

        apidep = API_render;	
    else
	apidep = api;

    strncpy(reqtyp, str, TYPBUFSIZ-1);
    reqdep = strchr(reqtyp, ':');
    if (reqdep) {
	*reqdep++ = '\0';
        reqpkg = strchr(reqdep, ':');
        if (reqpkg)
            *reqpkg++ = '\0';
    }
    else
	reqpkg = NULL;

    /* iterate the linked list of plugins for this api */
    for (pnext = &(gvc->apis[api]); *pnext; pnext = &((*pnext)->next)) {
        strncpy(typ, (*pnext)->typestr, TYPBUFSIZ-1);
	dep = strchr(typ, ':');
	if (dep) 
	    *dep++ = '\0';
	if (strcmp(typ, reqtyp)) 
	    continue;  /* types empty or mismatched */
 	if (dep && reqdep && strcmp(dep, reqdep))
	    continue;  /* dependencies not empty, but mismatched */
	if (! reqpkg)
	    break; /* found with no packagename constraints */
	if (strcmp(reqpkg, (*pnext)->package->name) == 0)
	    break;  /* found with required matching packagname */
    }
    rv = *pnext;

    if (dep && (apidep != api)) /* load dependency if needed */
	if (! (gvplugin_load(gvc, apidep, dep)))
	    rv = NULL;

    if (rv && rv->typeptr == NULL) {
	library = gvplugin_library_load(gvc, rv->package->path);
	if (library) {

            /* Now activate the library with real type ptrs */
            for (apis = library->apis; (types = apis->types); apis++) {
		for (i = 0; types[i].type; i++) {
		    /* NB. quality is not checked or replaced
 		     *   in case user has manually edited quality in config */
                    gvplugin_activate(gvc,
				apis->api,
				types[i].type,
				library->packagename,
				rv->package->path,
				&types[i]);
		}
            }
    	    if (gvc->common.verbose >= 1)
		fprintf(stderr, "Activated plugin library: %s\n",
			rv->package->path ? rv->package->path : "<builtin>");
        }
    }

    /* one last check for successfull load */
    if (rv && rv->typeptr == NULL)
	rv = NULL;

    if (rv && gvc->common.verbose >= 1)
	fprintf(stderr, "Using %s: %s:%s\n",
		api_names[api],
		rv->typestr,
		rv->package->name
		);

    gvc->api[api] = rv;
    return rv;
}

/* string buffer management
	- FIXME - must have 20 solutions for this same thing */
static char *append_buf(char sep, const char *str, boolean new)
{
    static char *buf;
    static int bufsz, pos;
    int len;
    char *p;

    if (new)
	pos = 0;
    len = strlen(str) + 1;
    if (bufsz < (pos + len + 1)) {
	bufsz += 4 * len;
	buf = grealloc(buf, bufsz);
    }
    p = buf + pos;
    *p++ = sep;
    strcpy(p, str);
    pos += len;
    return buf;
}

/* assemble a string list of available plugins */
char *gvplugin_list(GVC_t * gvc, api_t api, const char *str)
{
    gvplugin_available_t **pnext, **plugin;
    char *buf = NULL;
    char *s, *p, *q, *typestr_last;
    boolean new = TRUE;

    /* check for valid apis[] index */
    if (api < 0)
	return NULL;

    /* does str have a :path modifier? */
    s = strdup(str);
    p = strchr(s, ':');
    if (p)
	*p++ = '\0';

    /* point to the beginning of the linked list of plugins for this api */
    plugin = &(gvc->apis[api]);

    if (p) {	/* if str contains a ':', and if we find a match for the type,
		   then just list the alternative paths for the plugin */
	for (pnext = plugin; *pnext; pnext = &((*pnext)->next)) {
            q = strdup((*pnext)->typestr);
	    if ((p = strchr(q, ':')))
                *p++ = '\0';
	    /* list only the matching type, or all types if s is an empty string */
	    if (!s[0] || strcasecmp(s, q) == 0) {
		/* list each member of the matching type as "type:path" */
		append_buf(' ', (*pnext)->typestr, new);
		buf = append_buf(':', (*pnext)->package->name, FALSE);
		new = FALSE;
	    }
	    free(q);
	}
    }
    free(s);
    if (new) {			/* if the type was not found, or if str without ':',
				   then just list available types */
	typestr_last = NULL;
	for (pnext = plugin; *pnext; pnext = &((*pnext)->next)) {
	    /* list only one instance of type */
	    q = strdup((*pnext)->typestr);
	    if ((p = strchr(q, ':')))
		*p++ = '\0';
	    if (!typestr_last || strcasecmp(typestr_last, q) != 0) {
		/* list it as "type"  i.e. w/o ":path" */
		buf = append_buf(' ', q, new);
		new = FALSE;
	    }
	    if(!typestr_last)
		free(typestr_last);
	    typestr_last = q;
	}
	if(!typestr_last)
	    free(typestr_last);
    }
    if (!buf)
	buf = "";
    return buf;
}

void gvplugin_write_status(GVC_t * gvc)
{
    int api;

#ifdef ENABLE_LTDL
    if (Demand_Loading) {
        fprintf(stderr,"The plugin configuration file:\n\t%s\n", gvc->config_path);
        if (gvc->config_found)
	    fprintf(stderr,"\t\twas successfully loaded.\n");
        else
	    fprintf(stderr,"\t\twas not found or not usable. No on-demand plugins.\n");
    }
    else {
        fprintf(stderr,"Demand loading of plugins is disabled.\n");
    }
#endif

    for (api = 0; api < ARRAY_SIZE(api_names); api++) {
	if (gvc->common.verbose >= 2) 
	    fprintf(stderr,"    %s\t: %s\n", api_names[api], gvplugin_list(gvc, api, ":"));
	else
	    fprintf(stderr,"    %s\t: %s\n", api_names[api], gvplugin_list(gvc, api, "?"));
    }

}

Agraph_t * gvplugin_graph(GVC_t * gvc)
{
    Agraph_t *g, *sg, *ssg;
    Agnode_t *n, *m;
    Agedge_t *e;
    Agsym_t *a;
    gvplugin_package_t *package;
    gvplugin_available_t **pnext;
    char bufa[100], *buf1, *buf2, bufb[100], *p, *q;
    int api, found;

    aginit();
    /* set persistent attributes here */
    agnodeattr(NULL, "label", NODENAME_ESC);
    agraphattr(NULL, "label", "");
    agraphattr(NULL, "rankdir", "");
    agraphattr(NULL, "rank", "");

    g = agopen("G", AGDIGRAPH);

    a = agfindattr(g, "rankdir");
    agxset(g, a->index, "LR");

    a = agfindattr(g, "label");
    agxset(g, a->index, "\nPlugins");

    for (package = gvc->packages; package; package = package->next) {
        strcpy(bufa, "cluster_");
        strcat(bufa, package->name); 
	sg = agsubg(g, bufa);
        a = agfindattr(sg, "label");
	agxset(sg, a->index, package->name);
        strcpy(bufa, package->name); 
	strcat(bufa, "_");
	buf1 = bufa + strlen(bufa);
	for (api = 0; api < ARRAY_SIZE(api_names); api++) {
	    found = 0;
	    strcpy(buf1, api_names[api]);
	    ssg = agsubg(sg, bufa);
            a = agfindattr(ssg, "rank");
	    agxset(ssg, a->index, "same");
	    strcat(buf1, "_");
	    buf2 = bufa + strlen(bufa);
	    for (pnext = &(gvc->apis[api]); *pnext; pnext = &((*pnext)->next)) {
		if ((*pnext)->package == package) {
		    found++;
		    q = strdup((*pnext)->typestr);
		    if ((p = strchr(q, ':'))) *p++ = '\0';
		    switch (api) {
		    case API_device:
		    case API_loadimage:
		        strcpy(buf2, q);
		        n = agnode(ssg, bufa);
                        a = agfindattr(n, "label");
		        agxset(n, a->index, q);
			break;
		    case API_render:
			strcpy(bufb, api_names[api]);
		        strcat(bufb, "_");
		        strcat(bufb, q);
		        n = agnode(ssg, bufb);
                        a = agfindattr(n, "label");
		        agxset(n, a->index, q);
			break;
		    default:
			break;
		    }
		    free(q);
		}
	    }
	    if (!found)
	        agdelete(ssg->meta_node->graph, ssg->meta_node);
	}
    }

    for (package = gvc->packages; package; package = package->next) {
        strcpy(bufa, package->name); 
	strcat(bufa, "_");
	buf1 = bufa + strlen(bufa);
	for (api = 0; api < ARRAY_SIZE(api_names); api++) {
	    strcpy(buf1, api_names[api]);
	    strcat(buf1, "_");
	    buf2 = bufa + strlen(bufa);
	    for (pnext = &(gvc->apis[api]); *pnext; pnext = &((*pnext)->next)) {
		if ((*pnext)->package == package) {
		    q = strdup((*pnext)->typestr);
		    if ((p = strchr(q, ':'))) *p++ = '\0';
		    switch (api) {
		    case API_device:
		        strcpy(buf2, q);
			n = agnode(g, bufa);
		        strcpy(bufb, "o_");
		        strcat(bufb, q);
			m = agfindnode(g, bufb);
			if (!m) {
			    m = agnode(g, bufb);
			    a = agfindattr(m, "label");
		            agxset(m, a->index, q);
			}
			e = agfindedge(g, n, m);
			if (!e)
			    e = agedge(g, n, m);
			if (p && *p) {
			    strcpy(bufb, "render_");
			    strcat(bufb, p);
			    m = agnode(g, bufb);
			    agedge(g, m, n);
			}
			break;
		    case API_loadimage:
		        strcpy(buf2, q);
			n = agnode(g, bufa);
		        strcpy(bufb, "i_");
		        strcat(bufb, q);
			m = agfindnode(g, bufb);
			if (!m) {
			    m = agnode(g, bufb);
                            a = agfindattr(m, "label");
		            agxset(m, a->index, q);
			}
			e = agfindedge(g, m, n);
			if (!e)
			    e = agedge(g, m, n);
			strcpy(bufb, "render_");
			strcat(bufb, p);
			m = agnode(g, bufb); 
			agedge(g, n, m);
			break;
		    default:
			break;
		    }
		    free(q);
		}
	    }
	}
    }

    return g;
}
