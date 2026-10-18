/* Native provider editor. Drafts never change live settings until saved. */
static Provider pd_items[CW_MAX_PROVIDERS];
static int pd_count, pd_sel;
static const char *pd_types[] = { "http", "deepseek", "moonshot", "siliconflow", "openrouter" };
enum { PD_LIST=2000, PD_NAME, PD_TYPE, PD_KEY, PD_URL, PD_PATH, PD_PREFIX, PD_HEADERS,
       PD_ENABLED, PD_ADD, PD_DELETE, PD_SAVE, PD_CANCEL };
static HWND pd_control(HWND h, const wchar_t *cls, const wchar_t *txt, DWORD style,
                       int x, int y, int width, int height, int id) {
    HWND c = CreateWindowExW(!wcscmp(cls,L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, txt,
        WS_CHILD|WS_VISIBLE|style, x,y,width,height,h,(HMENU)(INT_PTR)id,g_inst,NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)ui_font(),TRUE);
    return c;
}
static void pd_set(HWND h, int id, const char *s) {
    wchar_t *v=cw_w_from_u8(s); SetDlgItemTextW(h,id,v?v:L""); free(v);
}
static void pd_get(HWND h,int id,char *out,int cap) {
    wchar_t buf[2048]; GetDlgItemTextW(h,id,buf,2048);
    char *v=cw_u8_from_w(buf); lstrcpynA(out,v?v:"",cap); free(v);
}
static void pd_store(HWND h) {
    if(pd_sel<0 || pd_sel>=pd_count) return;
    Provider *p=&pd_items[pd_sel];
    pd_get(h,PD_NAME,p->name,sizeof(p->name)); pd_get(h,PD_KEY,p->api_key,sizeof(p->api_key));
    pd_get(h,PD_URL,p->url,sizeof(p->url)); pd_get(h,PD_PATH,p->json_path,sizeof(p->json_path));
    pd_get(h,PD_PREFIX,p->prefix,sizeof(p->prefix)); pd_get(h,PD_HEADERS,p->headers,sizeof(p->headers));
    int t=(int)SendDlgItemMessageW(h,PD_TYPE,CB_GETCURSEL,0,0);
    lstrcpynA(p->type,pd_types[t>=0&&t<5?t:0],sizeof(p->type));
    p->enabled=SendDlgItemMessageW(h,PD_ENABLED,BM_GETCHECK,0,0)==BST_CHECKED;
}
static void pd_load(HWND h) {
    Provider empty={0}; Provider *p=pd_sel>=0&&pd_sel<pd_count?&pd_items[pd_sel]:&empty;
    pd_set(h,PD_NAME,p->name); pd_set(h,PD_KEY,p->api_key); pd_set(h,PD_URL,p->url);
    pd_set(h,PD_PATH,p->json_path); pd_set(h,PD_PREFIX,p->prefix); pd_set(h,PD_HEADERS,p->headers);
    int t=0; for(int i=0;i<5;i++) if(!strcmp(p->type,pd_types[i])) t=i;
    SendDlgItemMessageW(h,PD_TYPE,CB_SETCURSEL,t,0);
    SendDlgItemMessageW(h,PD_ENABLED,BM_SETCHECK,p->enabled?BST_CHECKED:BST_UNCHECKED,0);
    for(int id=PD_NAME;id<=PD_ENABLED;id++) EnableWindow(GetDlgItem(h,id),pd_sel>=0);
}
static void pd_list(HWND h) {
    SendDlgItemMessageW(h,PD_LIST,LB_RESETCONTENT,0,0);
    for(int i=0;i<pd_count;i++) {
        wchar_t *s=cw_w_from_u8(pd_items[i].name);
        SendDlgItemMessageW(h,PD_LIST,LB_ADDSTRING,0,(LPARAM)(s?s:L"未命名")); free(s);
    }
    SendDlgItemMessageW(h,PD_LIST,LB_SETCURSEL,pd_sel,0); pd_load(h);
}
static LRESULT CALLBACK ProviderDlgProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE: {
        pd_count=g_cfg.provider_count; memcpy(pd_items,g_cfg.providers,sizeof(pd_items)); pd_sel=pd_count?0:-1;
        pd_control(h,L"STATIC",L"服务商（最多 6 家）",0,16,14,174,22,0);
        pd_control(h,L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|WS_BORDER,16,40,174,340,PD_LIST);
        pd_control(h,L"BUTTON",L"添加",WS_TABSTOP,16,390,82,30,PD_ADD);
        pd_control(h,L"BUTTON",L"删除",WS_TABSTOP,106,390,84,30,PD_DELETE);
        const wchar_t *labels[]={L"显示名称",L"接口类型",L"API Key",L"余额接口 URL",L"余额取值路径",L"货币前缀",L"额外请求头"};
        for(int i=0;i<7;i++) {
            int id=PD_NAME+i, y=14+i*49;
            pd_control(h,L"STATIC",labels[i],0,210,y,420,19,0);
            if(id==PD_TYPE) {
                HWND c=pd_control(h,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,210,y+20,420,180,id);
                const wchar_t *names[]={L"自定义中转站（HTTP GET）",L"DeepSeek",L"Kimi / Moonshot",L"硅基流动",L"OpenRouter"};
                for(int n=0;n<5;n++) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)names[n]);
            } else {
                HWND c=pd_control(h,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(id==PD_KEY||id==PD_HEADERS?ES_PASSWORD:0),210,y+20,420,25,id);
                int limits[]={31,0,511,511,127,15,511};
                SendMessageW(c,EM_SETLIMITTEXT,limits[i],0);
            }
        }
        pd_control(h,L"BUTTON",L"启用此服务商",WS_TABSTOP|BS_AUTOCHECKBOX,210,363,220,24,PD_ENABLED);
        pd_control(h,L"STATIC",L"自定义接口：填写完整 HTTPS 余额查询地址，取值路径如 data.balance。\nKey 自动作为 Bearer 发送；额外请求头用 | 分隔。保存后立即刷新。",0,210,397,420,52,0);
        pd_control(h,L"BUTTON",L"保存并刷新",WS_TABSTOP|BS_DEFPUSHBUTTON,396,464,120,32,PD_SAVE);
        pd_control(h,L"BUTTON",L"取消",WS_TABSTOP,528,464,102,32,PD_CANCEL);
        pd_list(h); return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(w);
        if(id==PD_LIST && HIWORD(w)==LBN_SELCHANGE) {
            int next=(int)SendDlgItemMessageW(h,PD_LIST,LB_GETCURSEL,0,0); pd_store(h); pd_sel=next; pd_list(h); return 0;
        }
        if(id==PD_ADD) {
            if(pd_count==CW_MAX_PROVIDERS) {MessageBoxW(h,L"最多支持 6 家服务商。",L"提示",MB_OK);return 0;}
            pd_store(h); pd_sel=pd_count++; memset(&pd_items[pd_sel],0,sizeof(Provider));
            lstrcpyA(pd_items[pd_sel].name,"新中转站"); lstrcpyA(pd_items[pd_sel].type,"http");
            lstrcpyA(pd_items[pd_sel].json_path,"data.balance"); pd_items[pd_sel].enabled=1; pd_list(h); return 0;
        }
        if(id==PD_DELETE && pd_sel>=0) {
            if(MessageBoxW(h,L"删除选中的服务商？保存后生效。",L"删除服务商",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
            for(int i=pd_sel;i<pd_count-1;i++)pd_items[i]=pd_items[i+1];
            pd_count--; pd_sel=pd_count?0:-1; pd_list(h); return 0;
        }
        if(id==PD_SAVE) {
            pd_store(h);
            for(int i=0;i<pd_count;i++) {
                Provider *p=&pd_items[i];
                if(!p->name[0] || (!strcmp(p->type,"http") && p->enabled &&
                   (_strnicmp(p->url,"https://",8)||!p->json_path[0]))) {
                    pd_sel=i;pd_list(h);MessageBoxW(h,L"请填写名称。启用的自定义中转站需要完整 HTTPS URL 和余额取值路径。",L"请完善配置",MB_OK|MB_ICONWARNING);return 0;
                }
            }
            EnterCriticalSection(&g_lock);
            Config before=g_cfg; g_cfg.provider_count=pd_count; memcpy(g_cfg.providers,pd_items,sizeof(pd_items));
            int ok=cw_save_config();
            if(ok) memset(g_ps,0,sizeof(g_ps)); else g_cfg=before;
            LeaveCriticalSection(&g_lock);
            if(!ok) {MessageBoxW(h,L"保存失败：Windows 用户设置无法写入或解密，请检查当前用户权限。",L"配置未保存",MB_OK|MB_ICONWARNING);return 0;}
            SetEvent(g_ev_refresh); render_if_changed(1); DestroyWindow(h); return 0;
        }
        if(id==PD_CANCEL||id==IDCANCEL) {DestroyWindow(h);return 0;}
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
