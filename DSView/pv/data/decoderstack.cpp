#include "sigrok/decode-input.h"
/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
  

#include <stdexcept>
#include <algorithm>
#include <assert.h>
#include <sstream>

#include "decoderstack.h"
#include "dsosnapshot.h"
#include "../view/dsosignal.h"
#include "logicsnapshot.h"
#include "decode/decoder.h"
#include "decode/annotation.h"
#include "decode/rowdata.h"
#include "../sigsession.h"
#include "decodermodel.h"
#include "../view/logicsignal.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/langresource.h"
#include <ds_types.h>

using namespace pv::data::decode;
using namespace std;
using namespace boost;

namespace pv {
namespace data {

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 4 * 1024; 
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;
 
DecoderStack::DecoderStack(pv::SigSession *session,
	const srd_decoder *const dec, DecoderStatus *decoder_status) :
	_session(session)
{
    assert(session);
    assert(dec);
    assert(decoder_status); 
    
    _samples_decoded = 0;
    _sample_count = 0; 
    _decode_state = Stopped;
    _options_changed = false;
    _no_memory = false;
    _mark_index = -1;
    _decoder_status = decoder_status;
    _stask_stauts = NULL; 
    _is_capture_end = true;
    _snapshot = NULL;
    _dso_snapshot = NULL;
    _progress = 0;
    _is_decoding = false;
    _result_count = 0;
    _spi_message_recorded = false;
    _uart_message_recorded = false;
    
    _stack.push_back(new decode::Decoder(dec));
 
    build_row();
}

DecoderStack::~DecoderStack()
{   
    //release resource talbe
    DESTROY_OBJECT(_decoder_status);

    //release source
    for (auto &kv : _rows)
    {
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    //Decoder
    for (auto *p : _stack){
        delete p;
    }
    _stack.clear();
    
    _rows_gshow.clear();
    _rows_lshow.clear();
    _class_rows.clear();
}
 
void DecoderStack::add_sub_decoder(decode::Decoder *decoder)
{
	assert(decoder);
	_stack.push_back(decoder);
    build_row();
    _options_changed = true;
}

void DecoderStack::remove_sub_decoder(Decoder *decoder)
{
	// Find the decoder in the stack
    auto  iter = _stack.begin();
    for(unsigned int i = 0; i < _stack.size(); i++, iter++)
        if ((*iter) == decoder)
            break;

	// Delete the element
    if (iter != _stack.end())
    {
        _stack.erase(iter);
        delete decoder;
    }        

    build_row();
    _options_changed = true;
}

void DecoderStack::remove_decoder_by_handel(const srd_decoder *dec)
{
    Decoder *decoder = NULL;

    for (auto d : _stack){
        if (d->get_dec_handel() == dec){
            decoder = d;
            break;
        }
    }

    if (decoder){
        remove_sub_decoder(decoder);
    }
}

void DecoderStack::build_row()
{
    //release source
    for (auto &kv : _rows)
    {   
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    // Add classes
    for (auto dec : _stack)
    { 
        const srd_decoder *const decc = dec->decoder();
        assert(dec->decoder());

        dec->reset_start();

        // Add a row for the decoder if it doesn't have a row list
        if (!decc->annotation_rows) {
            const Row row(decc);
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }
        }

        // Add the decoder rows
        int order = 0;
        for (const GSList *l = decc->annotation_rows; l; l = l->next)
        {
            const srd_decoder_annotation_row *const ann_row =
                (srd_decoder_annotation_row *)l->data;
            assert(ann_row);

            const Row row(decc, ann_row, order);

            // Add a new empty row data object
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }

            // Map out all the classes
            for (const GSList *ll = ann_row->ann_classes; ll; ll = ll->next){
                _class_rows[make_pair(decc, GPOINTER_TO_INT(ll->data))] = Row(row);
            }

            order++;
        }
    }
}

int64_t DecoderStack::samples_decoded()
{
    std::lock_guard<std::mutex> decode_lock(_output_mutex);
	return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
	std::vector<pv::data::decode::Annotation*> &dest,
	const Row &row, uint64_t start_sample,
	uint64_t end_sample)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        (*iter).second->get_annotation_subset(dest,
			start_sample, end_sample);
}


uint64_t DecoderStack::get_annotation_index(
    const Row &row, uint64_t start_sample)
{  
    uint64_t index = 0;
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        index = (*iter).second->get_annotation_index(start_sample);

    return index;
}

uint64_t DecoderStack::get_max_annotation(const Row &row)
{ 
    auto iter =  _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_max_annotation();

    return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_min_annotation();

    return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow()
{
    std::map<const decode::Row, bool> rows_gshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_gshow.begin();
        i != _rows_gshow.end(); i++) {
        rows_gshow[(*i).first] = (*i).second;
    }
    return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow()
{
    std::map<const decode::Row, bool> rows_lshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_lshow.begin();
        i != _rows_lshow.end(); i++) {
        rows_lshow[(*i).first] = (*i).second;
    }
    return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
    if (iter != _rows_gshow.end()) {
        _rows_gshow[row] = show;
    }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_lshow.find(row);
    if (iter != _rows_lshow.end()) {
        _rows_lshow[row] = show;
    }
}

bool DecoderStack::has_annotations(const Row &row)
{  
    auto iter =
        _rows.find(row);
    if (iter != _rows.end())
        if(0 == (*iter).second->get_max_sample())
            return false;
        else
            return true;
    else
        return false;
}

uint64_t DecoderStack::list_annotation_size()
{
    std::lock_guard<std::mutex> lock(_output_mutex);
    uint64_t max_annotation_size = 0;

    for (auto it = _rows.begin(); it != _rows.end(); it++) {
        auto iter = _rows_lshow.find((*it).first);
        if (iter != _rows_lshow.end() && (*iter).second){
            max_annotation_size = max(max_annotation_size,
                (*it).second->get_annotation_size());
        }
    }

    return max_annotation_size;
}

uint64_t DecoderStack::list_annotation_size(uint16_t row_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            if (row_index-- == 0) {
                return (*i).second->get_annotation_size();
            }
    }
    return 0;
}

bool DecoderStack::list_annotation(pv::data::decode::Annotation *ann,
                                  uint16_t row_index, uint64_t col_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row_index-- == 0) {
                return (*i).second->get_annotation(ann, col_index);
            }
        }
    }

    return false;
}


bool DecoderStack::list_row_title(int row, QString &title)
{ 
    for (auto i = _rows.begin();i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row-- == 0) {
                title = (*i).first.title();
                return 1;
            }
        }
    }
    return 0;
}

void DecoderStack::clear()
{
    init();
    std::lock_guard<std::mutex> lock(_spi_messages_mutex);
    _spi_messages.clear();
    std::lock_guard<std::mutex> uart_lock(_uart_messages_mutex);
    _uart_messages.clear();
}

bool DecoderStack::has_spi_message_history() const
{
    const char *decoder_id = get_root_decoder_id();
    return decoder_id && (!strcmp(decoder_id, "0:spi") || !strcmp(decoder_id, "1:spi"));
}

uint64_t DecoderStack::spi_message_count() const
{
    std::lock_guard<std::mutex> lock(_spi_messages_mutex);
    return _spi_messages.size();
}

bool DecoderStack::spi_message(uint64_t index, SpiMessage &message) const
{
    std::lock_guard<std::mutex> lock(_spi_messages_mutex);
    if (index >= _spi_messages.size())
        return false;

    message = _spi_messages[index];
    return true;
}

bool DecoderStack::has_uart_message_history() const
{
    const char *decoder_id = get_root_decoder_id();
    if (!decoder_id)
        return false;

    const QString id = QString::fromUtf8(decoder_id);
    return id == "uart" || id.endsWith(":uart", Qt::CaseInsensitive);
}

uint64_t DecoderStack::uart_message_count() const
{
    std::lock_guard<std::mutex> lock(_uart_messages_mutex);
    return _uart_messages.size();
}

bool DecoderStack::uart_message(uint64_t index, UartMessage &message) const
{
    std::lock_guard<std::mutex> lock(_uart_messages_mutex);
    if (index >= _uart_messages.size())
        return false;

    message = _uart_messages[index];
    return true;
}

void DecoderStack::append_spi_message()
{
    if (!has_spi_message_history() || _spi_message_recorded)
        return;

    SpiMessage message;
    message.timestamp = _spi_message_timestamp;
    for (const auto &entry : _rows) {
        const QString title = entry.first.title();
        QString *output = title.endsWith(": MISO data") ? &message.miso :
            (title.endsWith(": MOSI data") ? &message.mosi : NULL);
        if (!output)
            continue;

        const uint64_t count = entry.second->get_annotation_size();
        dsv_info("SPI message row %s has %llu annotations.",
            title.toUtf8().constData(), (u64_t)count);
        for (uint64_t index = 0; index < count; index++) {
            Annotation annotation;
            if (!entry.second->get_annotation(&annotation, index) ||
                annotation.annotations().empty())
                continue;

            QString byte = annotation.annotations().front();
            if (byte.startsWith('@'))
                byte.remove(0, 1);
            if (!output->isEmpty())
                output->append(' ');
            output->append(byte);
        }
    }

    if (message.miso.isEmpty() && message.mosi.isEmpty()) {
        dsv_info("SPI message contained no complete data bytes.");
        return;
    }

    std::lock_guard<std::mutex> lock(_spi_messages_mutex);
    static const size_t MaxSpiMessages = 256;
    if (_spi_messages.size() == MaxSpiMessages)
        _spi_messages.erase(_spi_messages.begin());
    _spi_messages.push_back(message);
    _spi_message_recorded = true;
    dsv_info("SPI frame decoded at %s: MISO=[%s] MOSI=[%s]",
        message.timestamp.toUtf8().constData(),
        message.miso.toUtf8().constData(), message.mosi.toUtf8().constData());

    DecoderModel *model = _session->get_decoder_model();
    QMetaObject::invokeMethod(model, [model]() {
        model->refresh();
    }, Qt::QueuedConnection);
}

void DecoderStack::append_uart_message()
{
    if (!has_uart_message_history() || _uart_message_recorded)
        return;

    UartMessage message;
    message.timestamp = _spi_message_timestamp;
    for (const auto &entry : _rows) {
        if (!entry.first.title().endsWith(": RX/TX"))
            continue;

        const uint64_t count = entry.second->get_annotation_size();
        dsv_info("UART message row %s has %llu annotations.",
            entry.first.title().toUtf8().constData(), (u64_t)count);
        for (uint64_t index = 0; index < count; index++) {
            Annotation annotation;
            if (!entry.second->get_annotation(&annotation, index) ||
                annotation.format() != 0 || annotation.annotations().empty())
                continue;

            QString byte = annotation.annotations().front();
            if (byte.startsWith('@'))
                byte.remove(0, 1);
            if (!message.rxtx.isEmpty())
                message.rxtx.append(' ');
            message.rxtx.append(byte);
        }
    }

    if (message.rxtx.isEmpty())
        return;

    std::lock_guard<std::mutex> lock(_uart_messages_mutex);
    static const size_t MaxUartMessages = 256;
    if (_uart_messages.size() == MaxUartMessages)
        _uart_messages.erase(_uart_messages.begin());
    _uart_messages.push_back(message);
    _uart_message_recorded = true;
    dsv_info("UART frame decoded at %s: RX/TX=[%s]",
        message.timestamp.toUtf8().constData(), message.rxtx.toUtf8().constData());

    DecoderModel *model = _session->get_decoder_model();
    QMetaObject::invokeMethod(model, [model]() {
        model->refresh();
    }, Qt::QueuedConnection);
}

void DecoderStack::finalize_messages()
{
    if (IsRunning())
        return;

    append_spi_message();
    append_uart_message();
    if (_spi_message_recorded || _uart_message_recorded)
        new_decode_data();
}

void DecoderStack::init()
{
    _sample_count = 0; 
    _samples_decoded = 0;
    _error_message = QString();
    _no_memory = false;
    _snapshot = NULL;
    _result_count = 0;
    _spi_message_recorded = false;
    _uart_message_recorded = false;

    for (auto i = _rows.begin();i != _rows.end(); i++) { 
        (*i).second->clear();
    }

    set_mark_index(-1);
}
 
void DecoderStack::stop_decode_work()
{  
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
    _decode_state = Stopped; 
}

void DecoderStack::begin_decode_work()
{
     assert(_decode_state == Stopped);

     _error_message = "";
     _decode_state = Running;
      do_decode_work();
     _decode_state = Stopped;
}

bool DecoderStack::check_required_probes()
{
    for(auto dec : _stack){
		if (!dec->have_required_probes()) {
			return false;
		}
    }

    return true;
}

void DecoderStack::do_decode_work()
{
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
     _stask_stauts = new decode_task_status();
     _stask_stauts->_bStop = false;
     _stask_stauts->_decoder = this;
     _decoder_status->clear(); //clear old items

    if (!_options_changed)
    {  
        dsv_err("ERROR:Decoder options have not changed.");
        return;
    } 
    _options_changed = false;

    init();
    _spi_message_timestamp = _session->get_session_time().toString("yyyy-MM-dd HH:mm:ss.zzz");

    _snapshot = NULL;
    _dso_snapshot = NULL;

	// Check that all decoders have the required channels
    if (!check_required_probes()) {
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                            "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
	}

    const int signal_type = _session->get_device()->get_work_mode() == DSO ?
        SR_CHANNEL_DSO : SR_CHANNEL_LOGIC;

    for (auto dec : _stack) {
        if (dec->have_probes()) {
            for(auto s :  _session->get_signals()) {
                if (s->get_index() == dec->first_probe_index() && s->signal_type() == signal_type)
                { 
                    if (signal_type == SR_CHANNEL_DSO)
                        _dso_snapshot = ((pv::view::DsoSignal*)s)->data();
                    else
                        _snapshot = ((pv::view::LogicSignal*)s)->data();
                    if (_snapshot != NULL || _dso_snapshot != NULL)
                        break;
                }
            }
            if (_snapshot != NULL || _dso_snapshot != NULL)
                break;
        }
    }

    if (_snapshot == NULL && _dso_snapshot == NULL)
    {   
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                             "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
    }		

    if (_session->is_realtime_refresh() == false &&
        ((signal_type == SR_CHANNEL_DSO && _dso_snapshot->empty()) ||
         (signal_type == SR_CHANNEL_LOGIC && _snapshot->empty())))
    { 
        dsv_err("ERROR:Decode data is empty.");
        return;
    }

    // Get the samplerate
    _samplerate = signal_type == SR_CHANNEL_DSO ?
        _dso_snapshot->samplerate() : _snapshot->samplerate();
    if (_samplerate == 0.0)
    {
        dsv_err("ERROR:Decode data got an invalid sample rate.");
        return;
    }
     
    execute_decode_stack();   
}

uint64_t DecoderStack::get_max_sample_count()
{
	uint64_t max_sample_count = 0;

    for (auto i = _rows.begin(); i != _rows.end(); i++){
        max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
    } 	

	return max_sample_count;
}

void DecoderStack::decode_data(const uint64_t decode_start, const uint64_t decode_end, srd_session *const session, srd_decoder_inst *logic_di)
{
    decode_task_status *status = _stask_stauts;
    const bool spi_debug = logic_di->decoder &&
        (!strcmp(logic_di->decoder->id, "0:spi") || !strcmp(logic_di->decoder->id, "1:spi"));
  
    //uint8_t *chunk = NULL;
    uint64_t last_cnt = 0;
    uint64_t notify_cnt = (decode_end - decode_start + 1)/100;
    assert(logic_di);

    if (decode_end < decode_start || (_dso_snapshot && !_dso_snapshot->get_sample_count()))
        return;
    logic_di->abs_cur_samplenum = decode_start;
    uint64_t i = decode_start;
    char *error = NULL; 
    bool bError = false;
    bool bEndTime = false;

    if( i >= decode_end){
        dsv_info("decode data index have been to end");
    }

    std::vector<const uint8_t *> chunk;
    std::vector<uint8_t> chunk_const;
    bool bCheckEnd = false;
    uint64_t end_index = decode_end;
    uint64_t decoded_sample_count = 0;

    _progress = 0;
    _is_decoding = true;

    std::vector<void *> lbp_array(logic_di->dec_num_channels, nullptr);
    std::vector<uint8_t> dso_thresholds(logic_di->dec_num_channels);
    std::vector<int> spi_debug_last_values(logic_di->dec_num_channels, -1);
    std::vector<unsigned> spi_debug_transition_counts(logic_di->dec_num_channels, 0);
    std::vector<uint8_t> spi_debug_last_interleaved;

    if (_dso_snapshot) {
        const uint64_t sample_count = _dso_snapshot->get_sample_count();
        for (int j = 0; j < logic_di->dec_num_channels; j++) {
            const int sig_index = logic_di->dec_channelmap[j];
            if (sig_index == -1)
                continue;

            const uint8_t *samples = _dso_snapshot->get_samples(0, sample_count - 1, sig_index);
            uint8_t minimum = samples[0];
            uint8_t maximum = samples[0];
            for (uint64_t sample_index = 1; sample_index < sample_count; sample_index++) {
                minimum = std::min(minimum, samples[sample_index]);
                maximum = std::max(maximum, samples[sample_index]);
            }
            dso_thresholds[j] = minimum + (maximum - minimum) / 2;
            if (spi_debug)
                dsv_info("SPI debug threshold: decoder-channel=%d source-channel=%d min=%u max=%u threshold=%u high=(sample <= threshold)",
                    j, sig_index, minimum, maximum, dso_thresholds[j]);
        }
    }

    for (int j =0 ; j < logic_di->dec_num_channels; j++){
        lbp_array[j] = NULL;
    }

    while(i <= end_index && !_no_memory && !status->_bStop)
    {
        chunk.clear();
        chunk_const.clear();

        if (_is_capture_end)
        {
            if (!bCheckEnd){
                bCheckEnd = true;

                uint64_t align_sample_count = _dso_snapshot ?
                    _dso_snapshot->get_sample_count() : _snapshot->get_ring_sample_count();

                if (align_sample_count == 0){
                    dsv_info("Have no data to decode.");
                    return;
                }

                if (end_index >= align_sample_count){
                    end_index = align_sample_count - 1;
                    dsv_info("Reset the decode end sample index, new:%llu, old:%llu", 
                        (u64_t)end_index, (u64_t)decode_end);
                }

                if (i >= align_sample_count){
                    dsv_info("ERROR: the decoding sample index is out of range.");
                    break;
                }
            }
        }
        else if (i >= (_dso_snapshot ? _dso_snapshot->get_sample_count() :
                          _snapshot->get_ring_sample_count()))
        {   
            // Wait the data is ready.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
 
        uint64_t chunk_end = std::min(end_index + 1, i + MaxChunkSize);
        std::vector<std::vector<uint8_t>> dso_chunks(logic_di->dec_num_channels);
        std::vector<unsigned> bit_offsets(logic_di->dec_num_channels, 0);

        for (int j =0 ; j < logic_di->dec_num_channels; j++) {
            int sig_index = logic_di->dec_channelmap[j];
            void *lbp = NULL;

            if (sig_index == -1) {
                chunk.push_back(NULL);
                chunk_const.push_back(0);
            }
            else {
                if (_dso_snapshot && _dso_snapshot->has_data(sig_index)) {
                    const uint8_t *samples = _dso_snapshot->get_samples(i, chunk_end - 1, sig_index);
                    std::vector<uint8_t> &digital = dso_chunks[j];
                    digital.assign((chunk_end - i + 7) / 8, 0);
                    for (uint64_t sample_index = 0; sample_index < chunk_end - i; sample_index++) {
                        if (dsview_dso_sample_to_logic(samples[sample_index], dso_thresholds[j]))
                            digital[sample_index / 8] |= 1 << (sample_index % 8);
                    }
                    chunk.push_back(digital.data());
                    chunk_const.push_back(0);
                }
                else if (_snapshot && _snapshot->has_data(sig_index)) {
                    uint64_t logic_chunk_end = chunk_end;
                    const uint8_t *data_ptr = _snapshot->get_samples(i, logic_chunk_end, sig_index, &lbp);
                    chunk_end = std::min(chunk_end, logic_chunk_end);
                    bit_offsets[j] = i % 8;
                    bool flag = _snapshot->get_sample(i, sig_index);
                    chunk.push_back(data_ptr);
                    chunk_const.push_back(flag);

                    if (_snapshot->is_able_free() == false)
                    {
                        if (lbp_array[j] != lbp){
                            if (lbp_array[j] != NULL)
                                _snapshot->free_decode_lpb(lbp_array[j]);
                            lbp_array[j] = lbp;
                        }
                    }
                }
                else {
                    _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_DATA_ERROR),
                                     "At least one of selected channels are not enabled.");
                    return;
                }
            }
        }

        if (i > end_index){
            bEndTime = true;
            dsv_info("Decoding data to end.");
            break;
        }

        bEndTime = (chunk_end > end_index);

        unsigned highest_channel = 0;
        for (int j = 0; j < logic_di->dec_num_channels; j++)
            if (logic_di->dec_channelmap[j] >= 0)
                highest_channel = std::max(highest_channel, unsigned(logic_di->dec_channelmap[j]));
        const size_t unitsize = highest_channel / 8 + 1;
        auto interleaved = dsview_interleave(chunk.data(), chunk_const.data(),
            bit_offsets.data(), logic_di->dec_channelmap, logic_di->dec_num_channels,
            chunk_end - i, unitsize);
        int result = srd_session_send(session, i, chunk_end, interleaved.data(),
                                      interleaved.size(), unitsize);
        if (result != SRD_OK) {
            _error_message = QString::fromUtf8(srd_strerror(result));
            bError = true;
            break;
        }

        decoded_sample_count += chunk_end - i; 
        _progress = (int)(decoded_sample_count * 100 / (end_index - decode_start + 1));
        i = chunk_end;   
 
        //use mutex
        {
            std::lock_guard<std::mutex> lock(_output_mutex);
            _samples_decoded = i - decode_start + 1;
        }

        if ((i - last_cnt) > notify_cnt) {
            last_cnt = i;
            new_decode_data();
        }
    }

    // the task is normal ends,so all samples was processed;
    if (!bError && bEndTime){
       ds_srd_session_end(session, &error);

        if (error != NULL){
            _error_message = QString::fromLocal8Bit(error);
            dsv_err("Failed to call srd_session_end:%s", error);
        }
    }

    if (!bError && bEndTime) {
        append_spi_message();
        append_uart_message();
    }

    _progress = 100;
    _is_decoding = false;
    new_decode_data();

    if (error != NULL){
        g_free(error);
    }
  
    if (!_session->is_closed()){
        decode_done();
    }

    dsv_info("Decoded sample count:%llu", decoded_sample_count);
}

void DecoderStack::execute_decode_stack()
{  
	srd_session *session = NULL;
	srd_decoder_inst *prev_di = NULL;
    srd_decoder_inst *root_di = NULL;
    uint64_t decode_start = 0;
    uint64_t decode_end = 0;

    assert(_snapshot || _dso_snapshot);

	// Create the session
    // one decoderstatck onwer one session
    // all decoderstatck execute in sequence
	srd_session_new(&session);

    if (session == NULL){
        dsv_err("Failed to call srd_session_new()");
        assert(false);
    }
    
    // Get the intial sample count
    _sample_count = _dso_snapshot ? _dso_snapshot->get_sample_count() :
        _snapshot->get_ring_sample_count();
 
    // Create the decoders
    for(auto dec : _stack)
	{
        srd_decoder_inst *const di = dec->create_decoder_inst(session);

		if (!di)
		{
			_error_message =L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_STACK_ERROR), 
                            "Failed to create decoder instance");
			srd_session_destroy(session);
			return;
		}

        if (!root_di) root_di = di;
		if (prev_di)
			srd_inst_stack (session, prev_di, di);

		prev_di = di;
        decode_start = dec->decode_start();

        if (_session->is_realtime_refresh() == false)
            decode_end = min(dec->decode_end(), _sample_count-1);
        else
            decode_end = max(dec->decode_end(), decode_end);
	}

    dsv_info("Decode start sample index:%llu, end sample index:%llu, count:%llu", 
            (u64_t)decode_start, (u64_t)decode_end, (u64_t)(decode_end - decode_start + 1));

	// Start the session
	srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
		g_variant_new_uint64((uint64_t)_samplerate));

	srd_pd_output_callback_add(
                    session, 
                    SRD_OUTPUT_ANN,
		            DecoderStack::annotation_callback,
                    _stask_stauts);

    char *error = NULL;
    if (srd_session_start(session) == SRD_OK){
       //need a lot time
        decode_data(decode_start, decode_end, session, root_di);
    }
    else {
        _error_message = tr("Failed to start decoder session.");
    }

	// Destroy the session
    if (error != NULL) {
        g_free(error);
    }

	srd_session_destroy(session); 
}

uint64_t DecoderStack::sample_count()
{
    if (_snapshot)
        return _snapshot->get_sample_count();
    else
        return 0;
}

uint64_t DecoderStack::sample_rate()
{
    return _samplerate;
}

//the decode callback, annotation object will be create
void DecoderStack::annotation_callback(srd_proto_data *pdata, void *self)
{
	assert(pdata);
	assert(self);

    struct decode_task_status *st = (decode_task_status*)self;

	DecoderStack *const d = st->_decoder;
	assert(d);

    if (st->_bStop){ 
        return;
    }
    if (d->_decoder_status == NULL){ 
        dsv_err("decode task was deleted.");
        assert(false);
    }
  
    if (d->_no_memory) {
        return;
    }

    Annotation *a = new Annotation(pdata, d->_decoder_status);
    if (a == NULL){
        d->_no_memory = true;
        return;     
    }
    d->_result_count++;

	// Find the row
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

    auto row_iter = d->_rows.end();
	
	// Try looking up the sub-row of this class
	const map<pair<const srd_decoder*, int>, Row>::const_iterator r =
        d->_class_rows.find(make_pair(decc, a->format()));
	if (r != d->_class_rows.end())
        row_iter = d->_rows.find((*r).second);
	else
	{
		// Failing that, use the decoder as a key
        row_iter = d->_rows.find(Row(decc));
	}

    assert(row_iter != d->_rows.end());
    if (row_iter == d->_rows.end()) {
        dsv_err("Unexpected annotation: decoder = 0x%x, format = %d", (void*)decc, a->format());
        assert(0);
        return;
    }

	// Add the annotation 
    if (!(*row_iter).second->push_annotation(a))
        d->_no_memory = true; 
}
 
void DecoderStack::frame_ended()
{ 
    _options_changed = true; 
}

int DecoderStack::list_rows_size()
{ 
    int rows_size = 0;
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            rows_size++;
    }
    return rows_size;
}

bool DecoderStack::options_changed()
{
    return _options_changed;
}

void DecoderStack::set_options_changed(bool changed)
{
    _options_changed = changed;
}

bool DecoderStack::out_of_memory()
{
    return _no_memory;
}

void DecoderStack::set_mark_index(int64_t index)
{
    _mark_index = index;
}

int64_t DecoderStack::get_mark_index()
{
    return _mark_index;
}

const char* DecoderStack::get_root_decoder_id() const
{
    if (_stack.size() > 0){
        decode::Decoder *dec = _stack.front();
        return dec->decoder()->id;
    }
    return NULL;
}

} // namespace data
} // namespace pv
