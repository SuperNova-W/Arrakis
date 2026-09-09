import finbertManifest from '../../backend/models/finbert/manifest.json'
import xlbManifest from '../../backend/deploy/news_models/XLB.json.manifest.json'
import xlbMetrics from '../../backend/deploy/news_models/XLB.json.metrics.json'
import xlcManifest from '../../backend/deploy/news_models/XLC.json.manifest.json'
import xlcMetrics from '../../backend/deploy/news_models/XLC.json.metrics.json'
import xleManifest from '../../backend/deploy/news_models/XLE.json.manifest.json'
import xleMetrics from '../../backend/deploy/news_models/XLE.json.metrics.json'
import xlfManifest from '../../backend/deploy/news_models/XLF.json.manifest.json'
import xlfMetrics from '../../backend/deploy/news_models/XLF.json.metrics.json'
import xliManifest from '../../backend/deploy/news_models/XLI.json.manifest.json'
import xliMetrics from '../../backend/deploy/news_models/XLI.json.metrics.json'
import xlkManifest from '../../backend/deploy/news_models/XLK.json.manifest.json'
import xlkMetrics from '../../backend/deploy/news_models/XLK.json.metrics.json'
import xlpManifest from '../../backend/deploy/news_models/XLP.json.manifest.json'
import xlpMetrics from '../../backend/deploy/news_models/XLP.json.metrics.json'
import xlreManifest from '../../backend/deploy/news_models/XLRE.json.manifest.json'
import xlreMetrics from '../../backend/deploy/news_models/XLRE.json.metrics.json'
import xluManifest from '../../backend/deploy/news_models/XLU.json.manifest.json'
import xluMetrics from '../../backend/deploy/news_models/XLU.json.metrics.json'
import xlvManifest from '../../backend/deploy/news_models/XLV.json.manifest.json'
import xlvMetrics from '../../backend/deploy/news_models/XLV.json.metrics.json'
import xlyManifest from '../../backend/deploy/news_models/XLY.json.manifest.json'
import xlyMetrics from '../../backend/deploy/news_models/XLY.json.metrics.json'

type ModelManifest = { feature_names: string[]; symbol: string }
type ModelMetrics = {
  train_rows: number; validation_rows: number; test_rows: number
  validation: { roc_auc: number; accuracy: number; log_loss: number }
  test: { roc_auc: number; accuracy: number; log_loss: number }
  validation_start: string; validation_end: string; test_start: string; test_end: string
  selected_best_iteration: number; rounds_run: number
}

export type SignalModel = {
  symbol: string; signalModel: string; languageModel: string; languageModelVersion: string
  featureNames: string[]; marketFeatureCount: number; newsFeatureCount: number; textRepresentationCount: number
  trainingRows: number; validationRows: number; testRows: number
  validationAuc: number; validationAccuracy: number; validationLogLoss: number
  testAuc: number; testAccuracy: number; testLogLoss: number
  validationStart: string; validationEnd: string; testStart: string; testEnd: string
  selectedIteration: number; trainingRounds: number
}

function makeSignalModel(manifest: ModelManifest, metrics: ModelMetrics): SignalModel {
  const articleFeatureStart = manifest.feature_names.indexOf('article_count')
  const embeddingFeatureStart = manifest.feature_names.findIndex(name => name.startsWith('embedding_'))
  return {
    symbol: manifest.symbol, signalModel: 'XGBoost', languageModel: 'FinBERT',
    languageModelVersion: finbertManifest.artifact_id.replace(/^arrakis-finbert-/, ''),
    featureNames: manifest.feature_names, marketFeatureCount: articleFeatureStart,
    newsFeatureCount: manifest.feature_names.length - articleFeatureStart,
    textRepresentationCount: embeddingFeatureStart >= 0 ? manifest.feature_names.length - embeddingFeatureStart : 0,
    trainingRows: metrics.train_rows, validationRows: metrics.validation_rows, testRows: metrics.test_rows,
    validationAuc: metrics.validation.roc_auc, validationAccuracy: metrics.validation.accuracy,
    validationLogLoss: metrics.validation.log_loss, testAuc: metrics.test.roc_auc,
    testAccuracy: metrics.test.accuracy, testLogLoss: metrics.test.log_loss,
    validationStart: metrics.validation_start, validationEnd: metrics.validation_end,
    testStart: metrics.test_start, testEnd: metrics.test_end,
    selectedIteration: metrics.selected_best_iteration, trainingRounds: metrics.rounds_run,
  }
}

export const SECTOR_SIGNAL_MODELS: Record<string, SignalModel> = Object.fromEntries([
  [xlbManifest.symbol, makeSignalModel(xlbManifest, xlbMetrics)],
  [xlcManifest.symbol, makeSignalModel(xlcManifest, xlcMetrics)],
  [xleManifest.symbol, makeSignalModel(xleManifest, xleMetrics)],
  [xlfManifest.symbol, makeSignalModel(xlfManifest, xlfMetrics)],
  [xliManifest.symbol, makeSignalModel(xliManifest, xliMetrics)],
  [xlkManifest.symbol, makeSignalModel(xlkManifest, xlkMetrics)],
  [xlpManifest.symbol, makeSignalModel(xlpManifest, xlpMetrics)],
  [xlreManifest.symbol, makeSignalModel(xlreManifest, xlreMetrics)],
  [xluManifest.symbol, makeSignalModel(xluManifest, xluMetrics)],
  [xlvManifest.symbol, makeSignalModel(xlvManifest, xlvMetrics)],
  [xlyManifest.symbol, makeSignalModel(xlyManifest, xlyMetrics)],
])

export const XLK_SIGNAL_MODEL = SECTOR_SIGNAL_MODELS.XLK!
