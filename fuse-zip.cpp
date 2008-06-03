//TODO: insert license header

#define FUSE_USE_VERSION 26
#define PROGRAM "fuse-zip"
#define ERROR_STR_BUF_LEN 0x100

#include <fuse.h>
#include <zip.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <map>
#include <list>

using namespace std;

struct ltstr {
    bool operator() (const char* s1, const char* s2) const {
        return strcmp(s1, s2) < 0;
    }
};

class fusezip_node {
public:
    fusezip_node(int id, char *full_name) {
        this->id = id;
        this->full_name = full_name;
        this->name = full_name;
        this->is_dir=false;
    }

    ~fusezip_node() {
        free(full_name);
    }

    char *name, *full_name;
    bool is_dir;
    int id;
    list<fusezip_node*> childs;
};

typedef map <const char*, fusezip_node*, ltstr> filemap_t; 

class fusezip_data {
private:
    fusezip_node *root_node;

    inline void insert(fusezip_node *node) {
        files[node->full_name] = node;
    }
public:
    fusezip_data(struct zip *zip_file) {
        m_zip = zip_file;
        build_tree();
    }

    ~fusezip_data() {
        zip_close(m_zip);
        //TODO: handle error code of zip_close

        for (filemap_t::iterator i = files.begin(); i != files.end(); ++i) {
            delete i->second;
        }
    }

    void build_tree() {
        root_node = new fusezip_node(-1, strdup(""));
        root_node->is_dir = true;
        insert(root_node);

        int n = zip_get_num_files(m_zip);
        for (int i = 0; i < n; ++i) {
            char *fullname = strdup(zip_get_name(m_zip, i, 0));
            if (fullname == NULL) {
                //TODO: may be throw error?
                continue;
            }

            fusezip_node *node = new fusezip_node(i, fullname);
            char *lsl = fullname;
            while (*lsl++) {}
            while (lsl >= fullname && *lsl-- != '/') {}
            lsl++;
            // If the last symbol in file name is '/' then it is a directory
            if (*(lsl+1) == '\0') {
                // It will produce two \0s at the end of file name. I think that it is not a problem
                *lsl = '\0';
                node->is_dir = true;
                while (lsl >= fullname && *lsl-- != '/') {}
                lsl++;
            }

            // Adding new child to parent node. For items without '/' in fname it will be root_node.
            char c = *lsl;
            *lsl = '\0';
            filemap_t::iterator parent = files.find(fullname);
            assert(parent != files.end());
            parent->second->childs.push_back(node);
            *lsl = c;
            
            // Setting short name of node
            if (c == '/') {
                lsl++;
            }
            node->name = lsl;

            insert(node);
        }
    }

    filemap_t files;
    struct zip *m_zip;
};

static void *fusezip_init(struct fuse_conn_info *conn) {
    struct fuse_context *context = fuse_get_context();
    struct zip *zip_file = (struct zip*)context->private_data;

    fusezip_data *data = new fusezip_data(zip_file);
    return data;
}

static void fusezip_destroy(void *v_data) {
    fusezip_data *data = (fusezip_data*)v_data;
    delete data;
}

fusezip_node *get_file_node(const char *fname) {
    fusezip_data *data = (fusezip_data*)fuse_get_context()->private_data;
    filemap_t::iterator i = data->files.find(fname);
    if (i == data->files.end()) {
        return NULL;
    } else {
        return i->second;
    }
}

inline struct zip *get_zip() {
    fusezip_data *data = (fusezip_data*)fuse_get_context()->private_data;
    return data->m_zip;
}

static int fusezip_getattr(const char *path, struct stat *stbuf) {
    int res = 0;

    memset(stbuf, 0, sizeof(struct stat));
    if (*path == '\0') {
        return -ENOENT;
    }
    fusezip_node *node = get_file_node(path + 1);
    if (node == NULL) {
        return -ENOENT;
    }
    struct zip_stat zstat;
    zip_stat_index(get_zip(), node->id, 0, &zstat);
    if (node->is_dir) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2 + node->childs.size();
    } else {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
    }
    stbuf->st_ino = node->id;
    stbuf->st_size = zstat.size;
    stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = zstat.mtime;

    return res;
}

static int fusezip_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    if (*path == '\0') {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    
    fusezip_node *node = get_file_node(path + 1);
    if (node == NULL) {
        return -ENOENT;
    }
    for (list<fusezip_node*>::const_iterator i = node->childs.begin(); i != node->childs.end(); ++i) {
        filler(buf, (*i)->name, NULL, 0);
    }

    return 0;    
}

void print_usage() {
    printf("USAGE: %s <zip-file> [fusermount options]\n", PROGRAM);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return EXIT_FAILURE;
    }
    
    int err;
    struct zip *zip_file;
    //TODO: add ZIP_CREATE in rw version
    if ((zip_file = zip_open(argv[1], ZIP_CHECKCONS, &err)) == NULL) {
        char err_str[ERROR_STR_BUF_LEN];
        zip_error_to_str(err_str, ERROR_STR_BUF_LEN, err, errno);
        fprintf(stderr, "%s: cannot open zip archive %s: %s\n", PROGRAM, argv[1], err_str);
        return EXIT_FAILURE;
    }

    static struct fuse_operations fusezip_oper;
    fusezip_oper.init       =   fusezip_init;
    fusezip_oper.destroy    =   fusezip_destroy;
    fusezip_oper.readdir    =   fusezip_readdir;
    fusezip_oper.getattr    =   fusezip_getattr;
    return fuse_main(argc - 1, argv + 1, &fusezip_oper, zip_file);
}

