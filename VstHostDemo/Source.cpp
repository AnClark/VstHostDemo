#include <array>
#include <memory>

#include <windows.h>
#include <tchar.h>
#include <mmsystem.h>

#include <boost/atomic.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/optional.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/locks.hpp>

#include <balor/gui/all.hpp>
#include <balor/io/File.hpp>
#include <balor/system/all.hpp>
#include <balor/locale/all.hpp>
#include <balor/graphics/all.hpp>

#pragma warning(push)
#pragma warning(disable: 4996)
#include "./vstsdk2.4/pluginterfaces/vst2.x/aeffectx.h"
#pragma warning(pop)

#include "./HostApplication.hpp"
#include "./VstPlugin.hpp"
#include "./WaveOutProcessor.hpp"

namespace hwm {

namespace io = balor::io;
namespace gui = balor::gui;
namespace sys = balor::system;
namespace gpx = balor::graphics;

//! GUI�n�萔
static size_t CLIENT_WIDTH = 800;
static size_t CLIENT_HEIGHT = 200;

static size_t const KEY_HEIGHT = 50;
static size_t const KEY_WIDTH = 15;
static balor::Rectangle const KEYBOARD_RECT(0, CLIENT_HEIGHT - KEY_HEIGHT, CLIENT_WIDTH, KEY_HEIGHT);

//! �I�[�f�B�I�n�萔
static size_t const SAMPLING_RATE = 44100;
static size_t const BLOCK_SIZE = 1024;
static size_t const BUFFER_MULTIPLICITY = 4;

int main_impl()
{
	boost::mutex process_mutex;
	auto get_process_lock = [&] () -> boost::unique_lock<boost::mutex> { return boost::make_unique_lock(process_mutex); };

	gpx::Font font(L"���C���I", 18, gpx::Font::Style::regular, gpx::Font::Quality::antialiased);
	gpx::Font font_small(L"���C���I", 12, gpx::Font::Style::regular, gpx::Font::Quality::antialiased);

	//! ���C���E�B���h�E
	//! ���̊֐��̌㔼�ŁA
    //! ���Ղ�A�v���O����(VST�v���O�C���̃p�����[�^�̃v���Z�b�g)���X�g���ǉ������B
	gui::Frame frame(L"VstHostDemo", CLIENT_WIDTH, CLIENT_HEIGHT, gui::Frame::Style::singleLine);
	frame.icon(gpx::Icon::windowsLogo());
	frame.maximizeButton(false);

	//! ���[�h����VSTi�I��
	gui::OpenFileDialog  file_dialog;
	file_dialog.pathMustExist(true);
	file_dialog.filter(_T("VSTi DLL(*.dll)\n*.dll\nAll Files(*.*)\n*.*\n\n"));
	file_dialog.title(_T("Select a VSTi DLL"));
	bool selected = file_dialog.show(frame);
	if(!selected) { return 0; }

	//! VST�v���O�C���ƁA���[�h���Ă���VST�z�X�g�̊ԂŃf�[�^�����Ƃ肷��N���X
	HostApplication		hostapp(SAMPLING_RATE, BLOCK_SIZE);

	//! VstPlugin�N���X
	//! VST�v���O�C����C�C���^�[�t�F�[�X�ł���AEffect��ێ����āA���b�v���Ă���
	VstPlugin			vsti(file_dialog.filePath(), SAMPLING_RATE, BLOCK_SIZE, &hostapp);

	if(!vsti.IsSynth()) {
		gui::MessageBox::show(
			frame.handle(),
			_T("This plugin [") + 
			io::File(file_dialog.filePath()).name() +
			_T("] is an Audio Effect. VST Instrument is expected.")
			);
		return 0;
	}

	//! Wave�o�̓N���X
	//! Windows��Wave�I�[�f�B�I�f�o�C�X���I�[�v�����āA�I�[�f�B�I�̍Đ����s���B
	WaveOutProcessor	wave_out_;

	//! �f�o�C�X�I�[�v��
	bool const open_device =
		wave_out_.OpenDevice(
			SAMPLING_RATE, 
			2,	//2ch
			BLOCK_SIZE,				// �o�b�t�@�T�C�Y�B�Đ����r�؂�鎞�͂��̒l�𑝂₷�B���������C�e���V�͑傫���Ȃ�B
			BUFFER_MULTIPLICITY,	// �o�b�t�@���d�x�B�Đ����r�؂�鎞�͂��̒l�𑝂₷�B���������C�e���V�͑傫���Ȃ�B

			//! �f�o�C�X�o�b�t�@�ɋ󂫂�����Ƃ��ɌĂ΂��R�[���o�b�N�֐��B
			//! ���̃A�v���P�[�V�����ł́A���VstPlugin�ɑ΂��č����������s���A���������I�[�f�B�I�f�[�^��WaveOutProcessor�̍Đ��o�b�t�@�֏�������ł���B
			[&] (short *data, size_t device_channel, size_t sample) {

				auto lock = get_process_lock();

				//! VstPlugin�ɒǉ������m�[�g�C�x���g��
				//! �Đ��p�f�[�^�Ƃ��Ď��ۂ̃v���O�C�������ɓn��
				vsti.ProcessEvents();
				
				//! sample���̎��Ԃ̃I�[�f�B�I�f�[�^����
				float **syntheized = vsti.ProcessAudio(sample);

				size_t const channels_to_be_played = 
					std::min<size_t>(device_channel, vsti.GetEffect()->numOutputs);

				//! ���������f�[�^���I�[�f�B�I�f�o�C�X�̃`�����l�����ȓ��̃f�[�^�̈�ɏ����o���B
				//! �f�o�C�X�̃T���v���^�C�v��16bit�����ŊJ���Ă���̂ŁA
				//! VST����-1.0 .. 1.0�̃I�[�f�B�I�f�[�^��-32768 .. 32767�ɕϊ����Ă���B
				//! �܂��AVST���ō��������f�[�^�̓`�����l�����Ƃɗ񂪕�����Ă���̂ŁA
				//! Waveform�I�[�f�B�I�f�o�C�X�ɗ����O�ɃC���^�[���[�u����B
				for(size_t ch = 0; ch < channels_to_be_played; ++ch) {
					for(size_t fr = 0; fr < sample; ++fr) {
						double const sample = syntheized[ch][fr] * 32768.0;
						data[fr * device_channel + ch] =
							static_cast<short>(
								std::max<double>(-32768.0, std::min<double>(sample, 32767.0))
								);
					}
				}
			}
		);

	if(open_device == false) {
		return -1;
	}

	//! VST�́A��������Ȃǂ�char *�ň������Abalor�ł�Unicode�ň����B
	balor::String eff_name(vsti.GetEffectName(), balor::locale::Charset(932, true));

	//! frame�̕`��C�x���g�n���h��
	//! ���Ղ�`�悷��B
	frame.onPaint() = [eff_name] (gui::Frame::Paint& e) {
		e.graphics().pen(gpx::Color::black());

		//! draw keyboard
		for(size_t i = 0; i < 60; ++i) {
			switch(i % 12) {
			case 1: case 3: case 6: case 8: case 10:
				e.graphics().brush(gpx::Color(5, 5, 5));
				break;
			default:
				e.graphics().brush(gpx::Color(250, 240, 230));
			}

			e.graphics().drawRectangle(i * KEY_WIDTH, CLIENT_HEIGHT - KEY_HEIGHT, KEY_WIDTH, KEY_HEIGHT);
		}
	};

	//! �ȉ��̃}�E�X�C�x���g�n�̃n���h���́A���Օ������N���b�N�������̏����ȂǁB
	//! 
	//! MIDI�K�i�ł́A"���Ղ������ꂽ"�Ƃ������t�����u�m�[�g�I���v
	//! "���Ղ������ꂽ"�Ƃ������t�����u�m�[�g�I�t�v�Ƃ���MIDI���b�Z�[�W�Ƃ��Ē�`���Ă���B
	//! �����̂ق��A������ω�������u�s�b�`�x���h�v��A�y��̐ݒ��ς��A���F��ω������肷��u�R���g���[���`�F���W�v�ȂǁA
	//! ���܂��܂�MIDI���b�Z�[�W��K�؂ȃ^�C�~���O��VST�v���O�C���ɑ��邱�ƂŁAVST�v���O�C�������R�ɉ��t�ł���B
	//! 
	//! ���̃A�v���P�[�V�����ł́A��ʏ�ɕ`�悵�����Ղ��N���b�N���ꂽ����
	//! VstPlugin�Ƀm�[�g�I���𑗂�A���Տォ��N���b�N�����ꂽ���ɁA�m�[�g�I�t�𑗂��Ă���B
	boost::optional<size_t> sent_note;

	auto getNoteNumber = [] (balor::Point const &pt) -> boost::optional<size_t> {
		if( !KEYBOARD_RECT.contains(pt) ) {
			return boost::none;
		}

		return pt.x / 15 + 0x30; //! �L�[�{�[�h�̍��[�����Ղ�C3�ɂ��āA15px���ɔ����オ��B
	};

	frame.onMouseDown() = [&] (gui::Frame::MouseDown &e) {
		BOOST_ASSERT(!sent_note);

		if(!e.lButton() || e.ctrl() || e.shift()) {
			return;
		}

		auto note_number = getNoteNumber(e.position());
		if(!note_number) {
			return;
		}

		e.sender().captured(true);

        //! �v���O�C���Ƀm�[�g�I����ݒ�
		vsti.AddNoteOn(note_number.get());
		sent_note = note_number;
	};

	frame.onMouseMove() = [&] (gui::Frame::MouseEvent &e) {
		if(!sent_note) {
			return;
		}

		auto note_number = getNoteNumber(e.position());
		if(!note_number) {
			return;
		}

		if(note_number == sent_note) {
			return;
		}

		vsti.AddNoteOff(sent_note.get());
		vsti.AddNoteOn(note_number.get());
		sent_note = note_number;
	};

	frame.onMouseUp() = [&] (gui::Frame::MouseUp &e) {
		if(!sent_note) {
			return;
		}

		if(e.sender().captured()) {
			e.sender().captured(false);
		}

		vsti.AddNoteOff(sent_note.get());
		sent_note = boost::none;
	};

	frame.onDeactivate() = [&] (gui::Frame::Deactivate &/*e*/) {
		if(sent_note) {
			vsti.AddNoteOff(sent_note.get());
			sent_note = boost::none;
		}
	};

	//! �v���O�C�����̕`��
	gui::Panel plugin_name(frame, 10, 10, 125, 27);
	plugin_name.onPaint() = [&font, eff_name] (gui::Panel::Paint &e) {
		e.graphics().font(font);
		e.graphics().backTransparent(true);
		e.graphics().drawText(eff_name, e.sender().clientRectangle());
	};

	//! �v���O�������X�g�̐ݒu
	gui::Panel program_list_label(frame, 10, 80, 75, 18);
	program_list_label.onPaint() = [&font_small] (gui::Panel::Paint &e) {
		e.graphics().font(font_small);
		e.graphics().backTransparent(true);
		e.graphics().drawText(_T("Program List"), e.sender().clientRectangle());
	};

	std::vector<std::wstring> program_names(vsti.GetNumPrograms());
	for(size_t i = 0; i < vsti.GetNumPrograms(); ++i) {
		program_names[i] = balor::locale::Charset(932, true).decode(vsti.GetProgramName(i));
	}

	gui::ComboBox program_list(frame, 10, 100, 200, 20, program_names, gui::ComboBox::Style::dropDownList);
	program_list.list().font(font_small);
	program_list.onSelect() = [&] (gui::ComboBox::Select &e) {
		int const selected = e.sender().selectedIndex();
		if(selected != -1) {
			auto lock = get_process_lock();
			vsti.SetProgram(selected);
		}
	};

	//! �G�f�B�^�E�B���h�E
	gui::Frame editor;
	{
		//! ���[�h���Ă���VST�v���O�C�����g���G�f�B�^�E�B���h�E�������Ă���ꍇ�̂݁B
		if(vsti.HasEditor()) {
			editor = gui::Frame(eff_name, 400, 300, gui::Frame::Style::singleLine);
			editor.icon(gpx::Icon::windowsLogo());

			//���C���E�B���h�E�̉��ɕ\��
			editor.position(frame.position() + balor::Point(0, frame.size().height));
			editor.owner(&frame);
			editor.maximizeButton(false);	//! �G�f�B�^�E�B���h�E�̃T�C�Y�ύX�s��
			//! �G�f�B�^�E�B���h�E�͏����Ȃ��ōŏ�������̂�
			editor.onClosing() = [] (gui::Frame::Closing &e) {
				e.cancel(true);
				e.sender().minimized(true);
			};
			vsti.OpenEditor(editor);
		}
	}

	//! ���b�Z�[�W���[�v
	//! frame�����Ɣ�����
	frame.runMessageLoop();

	//! �I������
	vsti.CloseEditor();
	wave_out_.CloseDevice();

	return 0;
}

}	//::hwm

int APIENTRY WinMain(HINSTANCE , HINSTANCE , LPSTR , int ) {

	try {
		hwm::main_impl();
	} catch(std::exception &e) {
		balor::gui::MessageBox::show(
			balor::String(
				_T("error : ")) + 
				balor::locale::Charset(932, true).decode(e.what())
				);
	}
}
