#include "pch.h"

//--------------------------------------------------------------------

// デバッグ用コールバック関数。デバッグメッセージを出力する。
void ___outputLog(LPCTSTR text, LPCTSTR output)
{
	::OutputDebugString(output);
}

//--------------------------------------------------------------------

struct Check
{
	static const int32_t CheckRange = 0;
	static const int32_t CheckLastFrame = 1;
};

//--------------------------------------------------------------------

AviUtlInternal g_auin;
AviUtl::FilterPlugin* g_fp = 0;

//--------------------------------------------------------------------

struct Checker {
	AviUtl::FilterPlugin* fp;
	int32_t s; // 選択範囲の開始位置。
	int32_t e; // 選択範囲の終了位置。
	AviUtl::EditHandle* editp;
	int32_t frame_n = 0; // フレーム総数。
	int32_t frame_end = 0; // 最終フレーム位置。
	AviUtl::FileInfo fi = {};
	TCHAR text[1024] = {}; // チェックに引っかかったとき出力するテキスト。

	Checker(AviUtl::FilterPlugin* fp, int32_t s, int32_t e, AviUtl::EditHandle* editp)
		: fp(fp)
		, s(s)
		, e(e)
		, editp(editp)
	{
		// フレーム総数と最終フレーム位置はよく使うのでここで取得しておく。
		frame_n = fp->exfunc->get_frame_n(editp);
		frame_end = frame_n - 1;

		// フレームを時間に変換するのにファイル情報が必要になるのでここで取得しておく。
		fp->exfunc->get_file_info(editp, &fi);
	}

	/*
		このクラスでフレームを時間に変換する。
		そんなにたくさんの計算はしないので精度重視の double で処理する。
	*/
	struct Time {
		double time;
		Time(int32_t frame, const AviUtl::FileInfo& fi) {
			// ビデオレートとビデオスケールを使用してフレームを時間に変換する。
			// 除算するのでビデオレートは 0 かどうかチェックしておく。
			if (fi.video_rate == 0)
				this->time = 0;
			else
				this->time = (double)frame * fi.video_scale / fi.video_rate;
		}
		int hour(){ return (int)time / 3600; }
		int min(){ return (int)time / 60 % 60; }
		double getSec(){ return fmod(time, 60.0); }
	};

	/*
		以下の関数で Time クラスを使用して終了位置を時間に変換するとき、
		+1 してから渡した方が大体切りの良い数値になる事が多いのでそうしている。
		しかし、絶対そうなるというわけでもないし必須というわけでもない。これは好みの問題。
	*/

	/*
		出力範囲が指定されている場合は TRUE を返す。
		その場合はメッセージボックスに表示するテキストを text に追加する。
	*/
	BOOL checkRange()
	{
		if (s == 0 && e == frame_end)
			return FALSE; // 出力範囲は指定されていないので全体が出力される。

		{
			Time total(frame_end + 1, fi);
			Time selectStart(s, fi);
			Time selectEnd(e + 1, fi);

			TCHAR subText[MAX_PATH] = {};
			::StringCbPrintf(subText, sizeof(subText),
				_T("注意 : プロジェクトの一部分だけを出力するように設定されています\n")
				_T("全体の長さが %02d:%02d:%05.2f のプロジェクトに対して\n")
				_T("%02d:%02d:%05.2f～%02d:%02d:%05.2f の出力範囲が指定されています\n\n"),
				total.hour(), total.min(), total.getSec(),
				selectStart.hour(), selectStart.min(), selectStart.getSec(),
				selectEnd.hour(), selectEnd.min(), selectEnd.getSec());

			// text に subText を追加する。
			::StringCbCat(text, sizeof(text), subText);
		}

		return TRUE;
	}

	/*
		最終フレーム位置と全アイテムの最終位置が一致しない場合は TRUE を返す。
		その場合はメッセージボックスに表示するテキストを text に追加する。
	*/
	BOOL checkLastFrame()
	{
		if (g_auin.GetExEditFrameNumber() == 0)
			return FALSE; // 拡張編集の最終フレーム番号が無効の場合は何もしない。

		// 現在編集中のシーンのインデックスを取得する。
		int scene = g_auin.GetCurrentSceneIndex();

		// 現在編集中のシーンの中で最も後ろにあるオブジェクトの終了位置を取得する。
		int item_end = -1;
		{
			// オブジェクトの個数を取得する。
			int c = g_auin.GetCurrentSceneObjectCount();

			for (int i = 0; i < c; i++)
			{
				// オブジェクトを取得する。
				ExEdit::Object* object = g_auin.GetSortedObject(i);

				if (scene != object->scene_set)
					continue; // 現在のシーン内のオブジェクトではなかった。

				item_end = std::max(item_end, object->frame_end);
			}
		}

		// オブジェクトの終了位置が小さすぎる場合は何もしない。
		if (item_end <= 0)
			return FALSE;

		// オブジェクトの終了位置が最終フレーム位置より大きい場合は何もしない。
		if (item_end >= frame_end)
			return FALSE;

		{
			Time frameTime(frame_end + 1, fi);
			Time itemTime(item_end + 1, fi);

			TCHAR subText[MAX_PATH] = {};
			::StringCbPrintf(subText, sizeof(subText),
				_T("注意 : 最終フレーム位置と全アイテムの最終位置が一致しません\n")
				_T("%02d:%02d:%05.2f (最終フレーム位置)\n")
				_T("%02d:%02d:%05.2f (全アイテムの最終位置)\n\n"),
				frameTime.hour(), frameTime.min(), frameTime.getSec(),
				itemTime.hour(), itemTime.min(), itemTime.getSec());

			// text に subText を追加する。
			::StringCbCat(text, sizeof(text), subText);
		}

		return TRUE;
	}

	/*
		ユーザーが指定したチェックを実行する。
		チェックに引っかかった場合は処理を継続するかユーザーに問い合わせる。
		ユーザーが拒否した場合は FALSE を返す。それ以外の場合は TRUE を返す。
	*/
	BOOL check()
	{
		// 有効になっているチェックを実行する。
		if (fp->check[Check::CheckRange]) checkRange();
		if (fp->check[Check::CheckLastFrame]) checkLastFrame();

		// text の長さが 0 以外になっているなら、どれかのチェックに引っかかっている。
		if (_tcslen(text) != 0)
		{
			::StringCbCat(text, sizeof(text), _T("このまま出力を実行しますか？"));

			// ユーザーが出力を拒否した場合は FALSE を返す。
			if (IDNO == ::MessageBox(fp->hwnd, text, _T("AviUtl - コンフィグチェッカー"), MB_YESNO | MB_ICONEXCLAMATION))
				return FALSE;
		}

		return TRUE;
	}
};

//--------------------------------------------------------------------

DECLARE_HOOK_PROC(BOOL, __fastcall, aviutl_output, (AviUtl::EditHandle* editp, uint32_t flags));

/*
	本来は aviutl.exe の関数。動画の出力を始めようとすると呼び出される。
	これをフックして、まず設定が適正かチェックする。
	チェックに引っかかった場合はユーザーに動画の出力を継続するか問い合わせる。
	ユーザーが拒否した場合は動画の出力をキャンセルする。
	チェックに引っかからなかったり、ユーザーが継続を希望した場合は通常通り動画の出力を行う。
*/
IMPLEMENT_HOOK_PROC_NULL(BOOL, __fastcall, aviutl_output, (AviUtl::EditHandle* editp, uint32_t flags))
{
	// 「編集RAMプレビュー」の場合はデフォルトの処理を行う。
	if (flags == 0x10 && strcmp(editp->sav_3.name, "編集RAMプレビュー") == 0)
		return true_aviutl_output(editp, flags);

	// チェックに必要な必要な変数を取得する。
	// ただし、Checker のコンストラクタでも必要な変数を取得しているので、
	// 全部コンストラクタでやってもいいかもしれない。
	AviUtl::FilterPlugin* fp = g_fp;
	int32_t s = 0, e = 0;
	fp->exfunc->get_select_frame(editp, &s, &e);

	{
		// 動画を出力する前に設定が適正かどうかチェックする。

		Checker checker(fp, s, e, editp); // チェックに必要な変数を取得する。

		if (!checker.check()) // この中でチェックしている。
			return FALSE; // 動画の出力をキャンセルする。
	}

	// 通常通りの動画の出力を行う。
	// 本来なら __fastcall で呼んだあと __cdecl で返さなければならない。
	// しかし、引数が 2 個以下なら実質 0 個でそのまま返せるのでそうしている。
	return true_aviutl_output(editp, flags);
}

BOOL initHook()
{
	// 基本的に Detours を使用して関数をフックする。
	// その方が簡単だし、セキュリティチェックに引っかかりにくい気がする。
	DetourTransactionBegin();
	DetourUpdateThread(::GetCurrentThread());

	{
		// aviutl は 1.10 しか想定していない。

		// aviutl.exe のハンドルを取得する。
		// ただし、aviutl_dark.exe の可能性もあるので 0 を渡したほうが良い。
		uintptr_t aviutl = (uintptr_t)::GetModuleHandle(0);

		// aviutl で動画を出力しようとするとこの関数 (アドレス) が呼ばれる。
		true_aviutl_output = (Type_aviutl_output)(aviutl + 0x01A1A0);
		ATTACH_HOOK_PROC(aviutl_output);
	}

	if (DetourTransactionCommit() == NO_ERROR)
	{
		MY_TRACE(_T("API フックに成功しました\n"));
	}
	else
	{
		MY_TRACE(_T("API フックに失敗しました\n"));
	}

	return TRUE;
}

BOOL termHook()
{
	return TRUE;
}

//--------------------------------------------------------------------

BOOL func_init(AviUtl::FilterPlugin* fp)
{
	// ここの処理は拡張編集が先に読み込まれていることを前提にしている。
	// 本来だったらもっと厳密なタイミングでやらないとダメらしい。

	// 拡張編集の機能を使用できるようにする。ただし、0.92 しか想定していない。
	if (!g_auin.initExEditAddress())
		return FALSE;

	// fp はあとで使うのでグローバル変数に入れておく。
	g_fp = fp;

	// フックを仕掛ける。
	initHook();

	return TRUE;
}

BOOL func_exit(AviUtl::FilterPlugin* fp)
{
	termHook();

	return FALSE;
}

BOOL func_proc(AviUtl::FilterPlugin* fp, AviUtl::FilterProcInfo* fpip)
{
	return TRUE;
}

//--------------------------------------------------------------------

LPCSTR check_name[] = {
	"エンコード範囲をチェックする",
	"最終フレームをチェックする",
};
int check_def[] = {
	TRUE,
	TRUE,
};

EXTERN_C AviUtl::FilterPluginDLL* WINAPI GetFilterTable()
{
	// ここで ini ファイルなどから値を取得して
	// check_def を変更できるようにしたほうがいいかもしれない。

	LPCSTR name = "コンフィグチェッカー";
	LPCSTR information = "コンフィグチェッカー 1.0.0 by 蛇色";

	static AviUtl::FilterPluginDLL filter = {
		.flag =
			AviUtl::FilterPlugin::Flag::AlwaysActive |
			AviUtl::FilterPlugin::Flag::DispFilter |
			AviUtl::FilterPlugin::Flag::ExInformation,
		.name = name,
		.check_n = sizeof(check_name) / sizeof(*check_name),
		.check_name = check_name,
		.check_default = check_def,
		.func_proc = 0,//func_proc,
		.func_init = func_init,
		.func_exit = func_exit,
		.information = information,
	};

	return &filter;
}

//--------------------------------------------------------------------
