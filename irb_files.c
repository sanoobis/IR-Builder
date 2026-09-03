#include "irb_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char* error, const char* text) {
    snprintf(error, IRB_ERROR_SIZE, "%s", text);
    return false;
}
static bool report(IrbLoadProgress progress, void* context, IrbLoadPhase phase, uint32_t done,
                   uint32_t total) {
    return !progress || progress(phase, done, total, context);
}
static bool same(Storage* storage, const IrbCatalog* catalog, IrbLoadProgress progress,
                 void* context) {
    uint32_t hash, size;
    return irb_file_fingerprint(storage, catalog->path, &hash, &size, IrbLoadVerify, progress,
                                context) &&
           hash == catalog->hash && size == catalog->size;
}
bool irb_catalog_load(Storage* storage, IrbCatalog* catalog, const char* path, char* error,
                      IrbLoadProgress progress, void* context) {
    memset(catalog, 0, sizeof(*catalog));
    if(!irb_library_path_valid(path)) return fail(error, "Choose an SD-card .ir file.");
    snprintf(catalog->path, sizeof(catalog->path), "%s", path);
    if(!irb_file_fingerprint(storage, path, &catalog->hash, &catalog->size, IrbLoadHash, progress,
                             context))
        return fail(error, "Cannot read import file.");
    FlipperFormat* reader = flipper_format_buffered_file_alloc(storage);
    FuriString* name = furi_string_alloc();
    InfraredSignal* signal = infrared_signal_alloc();
    uint32_t version;
    bool ok = flipper_format_buffered_file_open_existing(reader, path);
    flipper_format_set_strict_mode(reader, true);
    ok = ok && flipper_format_read_header(reader, name, &version) && version == 1 &&
         (furi_string_equal(name, "IR signals file") || furi_string_equal(name, "IR library file"));
    while(ok) {
        if(!report(progress, context, IrbLoadIndex, flipper_format_tell(reader), catalog->size)) {
            ok = false;
            break;
        }
        if(infrared_signal_read_name(reader, name) != InfraredErrorCodeNone) {
            ok = flipper_format_tell(reader) + 1 >= catalog->size;
            break;
        }
        if(catalog->count == IRB_CATALOG_LIMIT) {
            ok = false;
            break;
        }
        IrbImportEntry* entry = &catalog->entries[catalog->count++];
        entry->offset = flipper_format_tell(reader);
        entry->usable = irb_name_valid(furi_string_get_cstr(name), false);
        snprintf(entry->name, sizeof(entry->name), "%s",
                 entry->usable ? furi_string_get_cstr(name) : "[invalid name]");
        ok = infrared_signal_read_body(signal, reader) == InfraredErrorCodeNone &&
             infrared_signal_is_valid(signal);
    }
    flipper_format_free(reader); // Close before fingerprinting the same path.
    infrared_signal_free(signal);
    furi_string_free(name);
    return (ok && catalog->count && same(storage, catalog, progress, context)) ||
           fail(error, "Invalid import file, changed file, or more than 256 buttons.");
}
static bool copy_source(Storage* storage, const IrbCatalog* catalog, char* target,
                        IrbLoadProgress progress, void* context) {
    if(!irb_storage_prepare(storage)) return false;
    bool available = false;
    for(unsigned i = 1; i <= 9999; ++i) {
        snprintf(target, IRB_PATH_SIZE, IRB_IMPORT_DIR "/IR_%04u.ir", i);
        if(storage_common_stat(storage, target, NULL) == FSE_NOT_EXIST) {
            available = true;
            break;
        }
    }
    if(!available) return false;
    File* input = storage_file_alloc(storage);
    File* output = storage_file_alloc(storage);
    bool created = false;
    bool ok = storage_file_open(input, catalog->path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) ok = created = storage_file_open(output, target, FSAM_WRITE, FSOM_CREATE_NEW);
    uint8_t bytes[512];
    uint32_t done = 0;
    while(ok && done < catalog->size) {
        size_t amount = catalog->size - done;
        if(amount > sizeof(bytes)) amount = sizeof(bytes);
        ok = report(progress, context, IrbLoadIndex, done, catalog->size) &&
             storage_file_read(input, bytes, amount) == amount &&
             storage_file_write(output, bytes, amount) == amount;
        done += amount;
    }
    storage_file_close(input);
    if(!storage_file_close(output)) ok = false;
    storage_file_free(input);
    storage_file_free(output);
    uint32_t hash, size;
    ok = ok &&
         irb_file_fingerprint(storage, target, &hash, &size, IrbLoadVerify, progress, context) &&
         hash == catalog->hash && size == catalog->size;
    if(!ok && created) storage_common_remove(storage, target);
    return ok;
}
bool irb_catalog_add(Storage* storage, const IrbCatalog* catalog, IrbProject* project, int entry,
                     uint32_t* added, char* error, IrbLoadProgress progress, void* context) {
    *added = 0;
    if(entry < -1 || entry >= (int)catalog->count || !irb_project_valid(project))
        return fail(error, "Invalid import selection.");
    if(!same(storage, catalog, progress, context)) return fail(error, "Import changed. Reopen it.");
    IrbProject* next = malloc(sizeof(*next));
    if(!next) return fail(error, "Not enough memory.");
    *next = *project;
    uint32_t source = next->import_count;
    for(unsigned i = 0; i < next->import_count; ++i) {
        if(next->imports[i].hash != catalog->hash || next->imports[i].size != catalog->size)
            continue;
        uint32_t hash, size;
        if(irb_file_fingerprint(storage, next->imports[i].path, &hash, &size, IrbLoadVerify,
                                progress, context) &&
           hash == catalog->hash && size == catalog->size) {
            source = i;
            break;
        }
    }
    bool ok = source < IRB_MAX_IMPORTS;
    for(unsigned i = 0; ok && i < catalog->count; ++i) {
        if(entry >= 0 && (unsigned)entry != i) continue;
        const IrbImportEntry* item = &catalog->entries[i];
        if(!item->usable || !irb_project_label_available(next, item->name, UINT32_MAX) ||
           next->extra_count == IRB_MAX_EXTRAS)
            continue;
        IrbExtraButton* extra = &next->extras[next->extra_count++];
        snprintf(extra->label, sizeof(extra->label), "%s", item->name);
        extra->offset = item->offset;
        extra->source = source;
        ++*added;
    }
    ok = ok && *added;
    bool created = false;
    if(ok && source == next->import_count) {
        IrbImportSource* target = &next->imports[source];
        ok = copy_source(storage, catalog, target->path, progress, context);
        created = ok;
        target->hash = catalog->hash;
        target->size = catalog->size;
        ++next->import_count;
    }
    ok = ok && same(storage, catalog, progress, context) && irb_project_valid(next) &&
         report(progress, context, IrbLoadVerify, 1, 1);
    if(ok)
        *project = *next;
    else if(created)
        storage_common_remove(storage, next->imports[source].path);
    free(next);
    return ok || fail(error, "Nothing imported. Check duplicate names, limits (32 buttons / 4 "
                             "files), and SD card.");
}

void irb_files_clear(IrbFileList* files) {
    for(unsigned i = 0; i < files->count; ++i)
        free(files->entries[i].name);
    free(files->entries);
    memset(files, 0, sizeof(*files));
}
static int file_compare(const void* left, const void* right) {
    const IrbFileEntry* a = left;
    const IrbFileEntry* b = right;
    if(!strcmp(a->name, "..")) return -1;
    if(!strcmp(b->name, "..")) return 1;
    if(a->directory != b->directory) return a->directory ? -1 : 1;
    const unsigned char* x = (const unsigned char*)a->name;
    const unsigned char* y = (const unsigned char*)b->name;
    while(*x && *y) {
        unsigned char cx = *x++, cy = *y++;
        if(cx >= 'A' && cx <= 'Z') cx += 'a' - 'A';
        if(cy >= 'A' && cy <= 'Z') cy += 'a' - 'A';
        if(cx != cy) return cx < cy ? -1 : 1;
    }
    if(*x != *y) return *x ? 1 : -1;
    return strcmp(a->name, b->name);
}
static void file_sort(IrbFileList* files) {
    // In-place heapsort: qsort is not exported by the Flipper FAP API.
    IrbFileEntry* entries = files->entries;
    for(unsigned i = 1; i < files->count; ++i) {
        unsigned child = i;
        while(child) {
            unsigned parent = (child - 1) / 2;
            if(file_compare(&entries[parent], &entries[child]) >= 0) break;
            IrbFileEntry swap = entries[parent];
            entries[parent] = entries[child];
            entries[child] = swap;
            child = parent;
        }
    }
    for(unsigned end = files->count; end > 1;) {
        --end;
        IrbFileEntry swap = entries[0];
        entries[0] = entries[end];
        entries[end] = swap;
        unsigned parent = 0;
        while(parent * 2 + 1 < end) {
            unsigned child = parent * 2 + 1;
            if(child + 1 < end && file_compare(&entries[child], &entries[child + 1]) < 0) ++child;
            if(file_compare(&entries[parent], &entries[child]) >= 0) break;
            swap = entries[parent];
            entries[parent] = entries[child];
            entries[child] = swap;
            parent = child;
        }
    }
}
static bool file_add(IrbFileList* files, const char* name, bool directory) {
    size_t length = strlen(name) + 1;
    if(files->count == IRB_FILE_LIMIT || files->name_bytes + length > IRB_FILE_NAME_BYTES)
        return false;
    if(files->count == files->capacity) {
        unsigned capacity = files->capacity + 32;
        IrbFileEntry* entries = realloc(files->entries, capacity * sizeof(*entries));
        if(!entries) return false;
        files->entries = entries;
        files->capacity = capacity;
    }
    char* copy = malloc(length);
    if(!copy) return false;
    memcpy(copy, name, length);
    files->entries[files->count++] = (IrbFileEntry){.name = copy, .directory = directory};
    files->name_bytes += length;
    return true;
}
bool irb_files_load(Storage* storage, IrbFileList* files, const char* path, bool projects,
                    char* error, IrbLoadProgress progress, void* context) {
    irb_files_clear(files);
    snprintf(files->path, sizeof(files->path), "%s", path);
    files->projects = projects;
    File* directory = storage_file_alloc(storage);
    bool ok = storage_dir_open(directory, path);
    FileInfo info;
    char name[IRB_PATH_SIZE];
    bool parent = !projects && !irb_path_equal(path, "/ext");
    while(ok) {
        bool is_dir;
        if(parent) {
            strcpy(name, "..");
            is_dir = true;
            parent = false;
        } else {
            if(!storage_dir_read(directory, &info, name, sizeof(name))) {
                // Flipper SD storage reports FSE_NOT_EXIST at normal directory EOF.
                FS_Error result = storage_file_get_error(directory);
                ok = result == FSE_OK || result == FSE_NOT_EXIST;
                break;
            }
            is_dir = (info.flags & FSF_DIRECTORY) != 0;
            if(!strcmp(name, ".") || !strcmp(name, "..")) continue;
            const char* extension = strrchr(name, '.');
            if(projects ? (is_dir || !extension || !irb_path_equal(extension, ".irb"))
                        : (!is_dir && (!extension || !irb_path_equal(extension, ".ir"))))
                continue;
        }
        if(!file_add(files, name, is_dir)) {
            ok = fail(error, "Folder too large for memory. Use smaller subfolders (512 entries / "
                             "16 KiB of names).");
            break;
        }
        ok = report(progress, context, IrbLoadIndex, 0, 0);
    }
    storage_dir_close(directory);
    storage_file_free(directory);
    if(ok) {
        file_sort(files);
    } else {
        irb_files_clear(files);
        if(!error[0]) fail(error, "Cannot read folder.");
    }
    return ok;
}
void irb_files_get_page(const IrbFileList* files, uint32_t start, IrbFilePage* page) {
    memset(page, 0, sizeof(*page));
    snprintf(page->path, sizeof(page->path), "%s", files->path);
    page->projects = files->projects;
    page->total = files->count;
    page->start = start < files->count ? start : 0;
    for(unsigned i = page->start; i < files->count && page->count < IRB_PAGE_SIZE; ++i) {
        snprintf(page->names[page->count], IRB_PATH_SIZE, "%s", files->entries[i].name);
        page->directories[page->count++] = files->entries[i].directory;
    }
}
bool irb_files_page(Storage* storage, IrbFilePage* page, char* error, IrbLoadProgress progress,
                    void* context) {
    IrbFileList files = {0};
    bool ok = irb_files_load(storage, &files, page->path, page->projects, error, progress, context);
    if(ok) irb_files_get_page(&files, page->start, page);
    irb_files_clear(&files);
    return ok;
}
