#define _XOPEN_SOURCE 700
#include "input.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

static char *prompt(EditLine *editor) {(void)editor;return "> ";}
static int read_character(EditLine *editor,wchar_t *out) {
    cli_input *input=NULL;
    el_get(editor,EL_CLIENTDATA,&input);
    mbstate_t state={0};
    for(;;) {
        unsigned char byte;
        if(*input->cancelled) return 0;
        ssize_t n=read(STDIN_FILENO,&byte,1);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return (int)n;
        if(!byte) {input->nul=1;*out=0xfffd;return 1;}
        char ch=(char)byte;
        size_t converted=mbrtowc(out,&ch,1,&state);
        if(converted==(size_t)-2) continue;
        if(converted==(size_t)-1) {input->invalid=1;*out=0xfffd;return 1;}
        return 1;
    }
}
static unsigned char erase_before_cursor(EditLine *editor,int key) {
    (void)key;
    const LineInfoW *line=el_wline(editor);
    el_wdeletestr(editor,(int)(line->cursor-line->buffer));
    return CC_REFRESH;
}
int cli_input_open(cli_input *input,volatile sig_atomic_t *cancelled) {
    memset(input,0,sizeof *input);
    setlocale(LC_CTYPE,"");
    if(MB_CUR_MAX==1) {
        if(!setlocale(LC_CTYPE,"C.UTF-8")) setlocale(LC_CTYPE,"en_US.UTF-8");
    }
    input->cancelled=cancelled;
    input->history=history_init();
    if(!input->history) return 0;
    HistEvent event;
    history(input->history,&event,H_SETSIZE,50);
    const char *term=getenv("TERM");
    if((!term || !*term || !strcmp(term,"dumb")) && setenv("TERM","xterm",1)) {
        history_end(input->history);input->history=NULL;return 0;
    }
    EditLine *editor=input->editor=el_init("dunlin",stdin,stderr,stderr);
    if(!editor) {history_end(input->history);input->history=NULL;return 0;}
    el_set(editor,EL_EDITOR,"emacs");
    el_set(editor,EL_SIGNAL,1);
    el_set(editor,EL_PROMPT,prompt);
    el_set(editor,EL_CLIENTDATA,input);
    el_wset(editor,EL_GETCFN,read_character);
    el_set(editor,EL_HIST,history,input->history);
    el_set(editor,EL_BIND,"^?","ed-delete-prev-char",NULL);
    el_set(editor,EL_BIND,"^H","ed-delete-prev-char",NULL);
    el_wset(editor,EL_ADDFN,L"cli-erase-before",L"Erase before the cursor",erase_before_cursor);
    el_set(editor,EL_BIND,"^U","cli-erase-before",NULL);
    return 1;
}
int cli_input_read(cli_input *input,size_t max,char **line) {
    *line=NULL;input->invalid=0;input->nul=0;
    EditLine *editor=input->editor;
    /* Enter editing mode before publishing the prompt. Otherwise a fast EOF
     * can arrive in canonical mode and be lost when libedit prepares the tty. */
    el_set(editor,EL_PREP_TERM,1);
    int count=0,status=0;
    const char *value=el_gets(editor,&count);
    if(value && count>0) {
        size_t n=strlen(value);
        if(n && value[n-1]=='\n') --n;
        if(input->nul || n>max) status=-3;
        else if(input->invalid) status=-2;
        else {
            *line=malloc(n+1);
            if(*line) {
                memcpy(*line,value,n);(*line)[n]=0;status=1;
                if(n) {HistEvent event;history(input->history,&event,H_ENTER,value);}
            } else status=-1;
        }
    } else if(count<0 && !*input->cancelled) status=-1;
    /* Restore cooked mode before approvals or tool execution. */
    el_set(editor,EL_PREP_TERM,0);
    return status;
}
void cli_input_close(cli_input *input) {
    if(input->editor) el_end(input->editor);
    if(input->history) history_end(input->history);
}
