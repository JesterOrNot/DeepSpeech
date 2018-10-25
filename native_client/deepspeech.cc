#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "deepspeech.h"
#include "alphabet.h"

#include "tensorflow/core/public/version.h"
#include "native_client/ds_version.h"

#include "tensorflow/core/public/session.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/util/memmapped_file_system.h"

#include "c_speech_features.h"

#include "ctcdecode/ctc_beam_search_decoder.h"

//TODO: infer batch size from model/use dynamic batch size
const unsigned int BATCH_SIZE = 1;

//TODO: use dynamic sample rate
const unsigned int SAMPLE_RATE = 16000;

const float AUDIO_WIN_LEN = 0.025f;
const float AUDIO_WIN_STEP = 0.01f;
const unsigned int AUDIO_WIN_LEN_SAMPLES = (unsigned int)(AUDIO_WIN_LEN * SAMPLE_RATE);
const unsigned int AUDIO_WIN_STEP_SAMPLES = (unsigned int)(AUDIO_WIN_STEP * SAMPLE_RATE);

const unsigned int MFCC_FEATURES = 26;

const float PREEMPHASIS_COEFF = 0.97f;
const unsigned int N_FFT = 512;
const unsigned int N_FILTERS = 26;
const unsigned int LOWFREQ = 0;
const unsigned int CEP_LIFTER = 22;

using namespace tensorflow;

using std::vector;

/* This is the actual implementation of the streaming inference API, with the
   Model class just forwarding the calls to this class.

   The streaming process uses three buffers that are fed eagerly as audio data
   is fed in. The buffers only hold the minimum amount of data needed to do a
   step in the acoustic model. The three buffers which live in StreamingContext
   are:

   - audio_buffer, used to buffer audio samples until there's enough data to
     compute input features for a single window.

   - mfcc_buffer, used to buffer input features until there's enough data for
     a single timestep. Remember there's overlap in the features, each timestep
     contains n_context past feature frames, the current feature frame, and
     n_context future feature frames, for a total of 2*n_context + 1 feature
     frames per timestep.

   - batch_buffer, used to buffer timesteps until there's enough data to compute
     a batch of n_steps.

   Data flows through all three buffers as audio samples are fed via the public
   API. When audio_buffer is full, features are computed from it and pushed to
   mfcc_buffer. When mfcc_buffer is full, the timestep is copied to batch_buffer.
   When batch_buffer is full, we do a single step through the acoustic model
   and accumulate results in StreamingState::accumulated_logits.

   When fininshStream() is called, we decode the accumulated logits and return
   the corresponding transcription.
*/
struct StreamingState {
  vector<float> accumulated_logits;
  vector<float> audio_buffer;
  float last_sample; // used for preemphasis
  vector<float> mfcc_buffer;
  vector<float> batch_buffer;
  bool skip_next_mfcc;
  ModelState* model;

  void feedAudioContent(const short* buffer, unsigned int buffer_size);
  char* intermediateDecode();
  char* finishStream();

  void processAudioWindow(const vector<float>& buf);
  void processMfccWindow(const vector<float>& buf);
  void pushMfccBuffer(const float* buf, unsigned int len);
  void addZeroMfccWindow();
  void processBatch(const vector<float>& buf, unsigned int n_steps);
};

struct ModelState {
  MemmappedEnv* mmap_env;
  Session* session;
  GraphDef graph_def;
  unsigned int ncep;
  unsigned int ncontext;
  Alphabet* alphabet;
  Scorer* scorer;
  unsigned int beam_width;
  unsigned int n_steps;
  unsigned int mfcc_feats_per_timestep;
  unsigned int n_context;

  ModelState();
  ~ModelState();

  /**
   * @brief Perform decoding of the logits, using basic CTC decoder or
   *        CTC decoder with KenLM enabled
   *
   * @param logits         Flat matrix of logits, of size:
   *                       n_frames * batch_size * num_classes
   *
   * @return String representing the decoded text.
   */
  char* decode(vector<float>& logits);

  /**
   * @brief Do a single inference step in the acoustic model, with:
   *          input=mfcc
   *          input_lengths=[n_frames]
   *
   * @param mfcc batch input data
   * @param n_frames number of timesteps in the data
   *
   * @param[out] output_logits Where to store computed logits.
   */
  void infer(const float* mfcc, unsigned int n_frames, vector<float>& output_logits);
};

ModelState::ModelState()
  : mmap_env(nullptr)
  , session(nullptr)
  , ncep(0)
  , ncontext(0)
  , alphabet(nullptr)
  , scorer(nullptr)
  , beam_width(0)
  , n_steps(-1)
  , mfcc_feats_per_timestep(-1)
  , n_context(-1)
{
}

ModelState::~ModelState()
{
  if (session) {
    Status status = session->Close();
    if (!status.ok()) {
      std::cerr << "Error closing TensorFlow session: " << status << std::endl;
    }
  }

  delete scorer;
  delete mmap_env;
  delete alphabet;
}

void
StreamingState::feedAudioContent(const short* buffer,
                                 unsigned int buffer_size)
{
  // Consume all the data that was passed in, processing full buffers if needed
  while (buffer_size > 0) {
    while (buffer_size > 0 && audio_buffer.size() < AUDIO_WIN_LEN_SAMPLES) {
      // Apply preemphasis to input sample and buffer it
      float sample = (float)(*buffer) - (PREEMPHASIS_COEFF * last_sample);
      audio_buffer.push_back(sample);
      last_sample = *buffer;
      ++buffer;
      --buffer_size;
    }

    // If the buffer is full, process and shift it
    if (audio_buffer.size() == AUDIO_WIN_LEN_SAMPLES) {
      processAudioWindow(audio_buffer);
      // Shift data by one step
      std::rotate(audio_buffer.begin(), audio_buffer.begin() + AUDIO_WIN_STEP_SAMPLES, audio_buffer.end());
      audio_buffer.resize(audio_buffer.size() - AUDIO_WIN_STEP_SAMPLES);
    }

    // Repeat until buffer empty
  }
}

char*
StreamingState::intermediateDecode()
{
  return model->decode(accumulated_logits);
}

char*
StreamingState::finishStream()
{
  // Flush audio buffer
  processAudioWindow(audio_buffer);

  // Add empty mfcc vectors at end of sample
  for (int i = 0; i < model->n_context; ++i) {
    addZeroMfccWindow();
  }

  // Process final batch
  if (batch_buffer.size() > 0) {
    processBatch(batch_buffer, batch_buffer.size()/model->mfcc_feats_per_timestep);
  }

  return model->decode(accumulated_logits);
}

void
StreamingState::processAudioWindow(const vector<float>& buf)
{
  skip_next_mfcc = !skip_next_mfcc;
  if (!skip_next_mfcc) { // Was true
    return;
  }

  // Compute MFCC features
  float* mfcc;
  int n_frames = csf_mfcc(buf.data(), buf.size(), SAMPLE_RATE,
                          AUDIO_WIN_LEN, AUDIO_WIN_STEP, MFCC_FEATURES, N_FILTERS, N_FFT,
                          LOWFREQ, SAMPLE_RATE/2, 0.f, CEP_LIFTER, 1, nullptr,
                          &mfcc);
  assert(n_frames == 1);

  pushMfccBuffer(mfcc, n_frames * MFCC_FEATURES);
  free(mfcc);
}

void
StreamingState::addZeroMfccWindow()
{
  static const float zero_buffer[MFCC_FEATURES] = {0.f};
  pushMfccBuffer(zero_buffer, MFCC_FEATURES);
}

void
StreamingState::pushMfccBuffer(const float* buf, unsigned int len)
{
  while (len > 0) {
    unsigned int next_copy_amount = std::min(len, (unsigned int)(model->mfcc_feats_per_timestep - mfcc_buffer.size()));
    mfcc_buffer.insert(mfcc_buffer.end(), buf, buf + next_copy_amount);
    buf += next_copy_amount;
    len -= next_copy_amount;
    assert(mfcc_buffer.size() <= model->mfcc_feats_per_timestep);

    if (mfcc_buffer.size() == model->mfcc_feats_per_timestep) {
      processMfccWindow(mfcc_buffer);
      // Shift data by one step of one mfcc feature vector
      std::rotate(mfcc_buffer.begin(), mfcc_buffer.begin() + MFCC_FEATURES, mfcc_buffer.end());
      mfcc_buffer.resize(mfcc_buffer.size() - MFCC_FEATURES);
    }
  }
}

void
StreamingState::processMfccWindow(const vector<float>& buf)
{
  auto start = buf.begin();
  auto end = buf.end();
  while (start != end) {
    unsigned int next_copy_amount = std::min<unsigned int>(std::distance(start, end), (unsigned int)(model->n_steps * model->mfcc_feats_per_timestep - batch_buffer.size()));
    batch_buffer.insert(batch_buffer.end(), start, start + next_copy_amount);
    start += next_copy_amount;
    assert(batch_buffer.size() <= model->n_steps * model->mfcc_feats_per_timestep);

    if (batch_buffer.size() == model->n_steps * model->mfcc_feats_per_timestep) {
      processBatch(batch_buffer, model->n_steps);
      batch_buffer.resize(0);
    }
  }
}

void
StreamingState::processBatch(const vector<float>& buf, unsigned int n_steps)
{
  model->infer(buf.data(), n_steps, accumulated_logits);
}

void
ModelState::infer(const float* aMfcc, unsigned int n_frames, vector<float>& logits_output)
{
  const size_t num_classes = alphabet->GetSize() + 1; // +1 for blank

  Tensor input(DT_FLOAT, TensorShape({BATCH_SIZE, n_steps, 2*n_context+1, MFCC_FEATURES}));

  auto input_mapped = input.flat<float>();
  int i;
  for (i = 0; i < n_frames*mfcc_feats_per_timestep; ++i) {
    input_mapped(i) = aMfcc[i];
  }
  for (; i < n_steps*mfcc_feats_per_timestep; ++i) {
    input_mapped(i) = 0;
  }

  Tensor input_lengths(DT_INT32, TensorShape({1}));
  input_lengths.scalar<int>()() = n_frames;

  vector<Tensor> outputs;
  Status status = session->Run(
    {{"input_node", input}, {"input_lengths", input_lengths}},
    {"logits"}, {}, &outputs);

  if (!status.ok()) {
    std::cerr << "Error running session: " << status << "\n";
    return;
  }

  auto logits_mapped = outputs[0].flat<float>();
  // The CTCDecoder works with log-probs.
  for (int t = 0; t < n_frames * BATCH_SIZE * num_classes; ++t) {
    logits_output.push_back(logits_mapped(t));
  }
}

char*
ModelState::decode(vector<float>& logits)
{
  const int cutoff_top_n = 40;
  const double cutoff_prob = 1.0;
  const size_t num_classes = alphabet->GetSize() + 1; // +1 for blank
  const int n_frames = logits.size() / (BATCH_SIZE * num_classes);

  vector<vector<double>> inputs;
  inputs.resize(n_frames);
  for (int t = 0; t < n_frames; ++t) {
    for (int i = 0; i < num_classes; ++i) {
      inputs[t].push_back(logits[t * num_classes + i]);
    }
  }

  // Vector of <probability, Output(tokens, timings)> pairs
  vector<std::pair<double, Output>> out = ctc_beam_search_decoder(
    inputs, *alphabet, beam_width, cutoff_prob, cutoff_top_n, scorer);

  return strdup(alphabet->LabelsToString(out[0].second.tokens).c_str());
}

int
DS_CreateModel(const char* aModelPath,
               unsigned int aNCep,
               unsigned int aNContext,
               const char* aAlphabetConfigPath,
               unsigned int aBeamWidth,
               ModelState** retval)
{
  std::unique_ptr<ModelState> model(new ModelState());
  model->mmap_env   = new MemmappedEnv(Env::Default());
  model->ncep       = aNCep;
  model->ncontext   = aNContext;
  model->alphabet   = new Alphabet(aAlphabetConfigPath);
  model->beam_width = aBeamWidth;

  *retval = nullptr;

  DS_PrintVersions();

  if (!aModelPath || strlen(aModelPath) < 1) {
    std::cerr << "No model specified, cannot continue." << std::endl;
    return error::INVALID_ARGUMENT;
  }

  Status status;
  SessionOptions options;

  bool is_mmap = std::string(aModelPath).find(".pbmm") != std::string::npos;
  if (!is_mmap) {
    std::cerr << "Warning: reading entire model file into memory. Transform model file into an mmapped graph to reduce heap usage." << std::endl;
  } else {
    status = model->mmap_env->InitializeFromFile(aModelPath);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      return status.code();
    }

    options.config.mutable_graph_options()
      ->mutable_optimizer_options()
      ->set_opt_level(::OptimizerOptions::L0);
    options.env = model->mmap_env;
  }

  status = NewSession(options, &model->session);
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.code();
  }

  if (is_mmap) {
    status = ReadBinaryProto(model->mmap_env,
                             MemmappedFileSystem::kMemmappedPackageDefaultGraphDef,
                             &model->graph_def);
  } else {
    status = ReadBinaryProto(Env::Default(), aModelPath, &model->graph_def);
  }
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.code();
  }

  status = model->session->Create(model->graph_def);
  if (!status.ok()) {
    std::cerr << status << std::endl;
    return status.code();
  }

  for (int i = 0; i < model->graph_def.node_size(); ++i) {
    NodeDef node = model->graph_def.node(i);
    if (node.name() == "input_node") {
      const auto& shape = node.attr().at("shape").shape();
      model->n_steps = shape.dim(1).size();
      model->n_context = (shape.dim(2).size()-1)/2;
      model->mfcc_feats_per_timestep = shape.dim(2).size() * shape.dim(3).size();
    } else if (node.name() == "logits_shape") {
      Tensor logits_shape = Tensor(DT_INT32, TensorShape({3}));
      if (!logits_shape.FromProto(node.attr().at("value").tensor())) {
        continue;
      }

      int final_dim_size = logits_shape.vec<int>()(2) - 1;
      if (final_dim_size != model->alphabet->GetSize()) {
        std::cerr << "Error: Alphabet size does not match loaded model: alphabet "
                  << "has size " << model->alphabet->GetSize()
                  << ", but model has " << final_dim_size
                  << " classes in its output. Make sure you're passing an alphabet "
                  << "file with the same size as the one used for training."
                  << std::endl;
        return error::INVALID_ARGUMENT;
      }
    }
  }

  if (model->n_context == -1) {
    std::cerr << "Error: Could not infer context window size from model file. "
              << "Make sure input_node is a 3D tensor with the last dimension "
              << "of size MFCC_FEATURES * ((2 * context window) + 1). If you "
              << "changed the number of features in the input, adjust the "
              << "MFCC_FEATURES constant in " __FILE__
              << std::endl;
    return error::INVALID_ARGUMENT;
  }

  *retval = model.release();
  return tensorflow::error::OK;
}

void
DS_DestroyModel(ModelState* ctx)
{
  delete ctx;
}

int
DS_EnableDecoderWithLM(ModelState* aCtx,
                       const char* aAlphabetConfigPath,
                       const char* aLMPath,
                       const char* aTriePath,
                       float aLMWeight,
                       float aValidWordCountWeight)
{
  try {
    aCtx->scorer = new Scorer(aLMWeight, aValidWordCountWeight,
                              aLMPath ? aLMPath : "",
                              aTriePath ? aTriePath : "",
                              *aCtx->alphabet);
    return 0;
  } catch (...) {
    return 1;
  }
}

char*
DS_SpeechToText(ModelState* aCtx,
                const short* aBuffer,
                unsigned int aBufferSize,
                unsigned int aSampleRate)
{
  StreamingState* ctx;
  int status = DS_SetupStream(aCtx, 0, aSampleRate, &ctx);
  if (status != tensorflow::error::OK) {
    return nullptr;
  }
  DS_FeedAudioContent(ctx, aBuffer, aBufferSize);
  return DS_FinishStream(ctx);
}

int
DS_SetupStream(ModelState* aCtx,
               unsigned int aPreAllocFrames,
               unsigned int aSampleRate,
               StreamingState** retval)
{
  *retval = nullptr;

  Status status = aCtx->session->Run({}, {}, {"initialize_state"}, nullptr);
  if (!status.ok()) {
    std::cerr << "Error running session: " << status << std::endl;
    return status.code();
  }

  std::unique_ptr<StreamingState> ctx(new StreamingState());
  if (!ctx) {
    std::cerr << "Could not allocate streaming state." << std::endl;
    return status.code();
  }

  const size_t num_classes = aCtx->alphabet->GetSize() + 1; // +1 for blank

  // Default initial allocation = 3 seconds.
  if (aPreAllocFrames == 0) {
    aPreAllocFrames = 150;
  }

  ctx->accumulated_logits.reserve(aPreAllocFrames * BATCH_SIZE * num_classes);

  ctx->audio_buffer.reserve(AUDIO_WIN_LEN_SAMPLES);
  ctx->last_sample = 0;
  ctx->mfcc_buffer.reserve(aCtx->mfcc_feats_per_timestep);
  ctx->mfcc_buffer.resize(MFCC_FEATURES*aCtx->n_context, 0.f);
  ctx->batch_buffer.reserve(aCtx->n_steps * aCtx->mfcc_feats_per_timestep);

  ctx->skip_next_mfcc = false;

  ctx->model = aCtx;

  *retval = ctx.release();
  return tensorflow::error::OK;
}

void
DS_FeedAudioContent(StreamingState* aSctx,
                    const short* aBuffer,
                    unsigned int aBufferSize)
{
  aSctx->feedAudioContent(aBuffer, aBufferSize);
}

char*
DS_IntermediateDecode(StreamingState* aSctx)
{
  return aSctx->intermediateDecode();
}

char*
DS_FinishStream(StreamingState* aSctx)
{
  char* str = aSctx->finishStream();
  DS_DiscardStream(aSctx);
  return str;
}

void
DS_DiscardStream(StreamingState* aSctx)
{
  delete aSctx;
}

void
DS_AudioToInputVector(const short* aBuffer,
                      unsigned int aBufferSize,
                      unsigned int aSampleRate,
                      unsigned int aNCep,
                      unsigned int aNContext,
                      float** aMfcc,
                      int* aNFrames,
                      int* aFrameLen)
{
  const int contextSize = aNCep * aNContext;
  const int frameSize = aNCep + (2 * aNCep * aNContext);

  // Compute MFCC features
  float* mfcc;
  int n_frames = csf_mfcc(aBuffer, aBufferSize, aSampleRate,
                          AUDIO_WIN_LEN, AUDIO_WIN_STEP, aNCep, N_FILTERS, N_FFT,
                          LOWFREQ, aSampleRate/2, PREEMPHASIS_COEFF, CEP_LIFTER,
                          1, NULL, &mfcc);

  // Take every other frame (BiRNN stride of 2) and add past/future context
  int ds_input_length = (n_frames + 1) / 2;
  // TODO: Use MFCC of silence instead of zero
  float* ds_input = (float*)calloc(ds_input_length * frameSize, sizeof(float));
  for (int i = 0, idx = 0, mfcc_idx = 0; i < ds_input_length;
       i++, idx += frameSize, mfcc_idx += aNCep * 2) {
    // Past context
    for (int j = aNContext; j > 0; j--) {
      int frame_index = (i - j) * 2;
      if (frame_index < 0) { continue; }
      int mfcc_base = frame_index * aNCep;
      int base = (aNContext - j) * aNCep;
      for (int k = 0; k < aNCep; k++) {
        ds_input[idx + base + k] = mfcc[mfcc_base + k];
      }
    }

    // Present context
    for (int j = 0; j < aNCep; j++) {
      ds_input[idx + j + contextSize] = mfcc[mfcc_idx + j];
    }

    // Future context
    for (int j = 1; j <= aNContext; j++) {
      int frame_index = (i + j) * 2;
      if (frame_index >= n_frames) { break; }
      int mfcc_base = frame_index * aNCep;
      int base = contextSize + aNCep + ((j - 1) * aNCep);
      for (int k = 0; k < aNCep; k++) {
        ds_input[idx + base + k] = mfcc[mfcc_base + k];
      }
    }
  }

  // Free mfcc array
  free(mfcc);

  if (aMfcc) {
    *aMfcc = ds_input;
  }
  if (aNFrames) {
    *aNFrames = ds_input_length;
  }
  if (aFrameLen) {
    *aFrameLen = frameSize;
  }
}

void
DS_PrintVersions() {
  std::cerr << "TensorFlow: " << tf_git_version() << std::endl;
  std::cerr << "DeepSpeech: " << ds_git_version() << std::endl;
}

